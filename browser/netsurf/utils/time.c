/*
 * Copyright 2007 Rob Kendrick <rjek@netsurf-browser.org>
 * Copyright 2004-2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2003 John M Bell <jmb202@ecs.soton.ac.uk>
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

/**
 * \file utils/time.c
 * \brief Implementation of time operations.
 */

/*
 * On Mac OS 9, utils/time.h shadows MSL's <time.h>, so gmtime and
 * struct tm are never defined via includes.  Declare them locally.
 */
#ifndef _TIME_H
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};
struct tm *gmtime(const long *timer);
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef WITH_CURL
#include <curl/curl.h>
#endif

#include "utils/ascii.h"
#include "utils/errors.h"
#include "utils/time.h"


/**
 * Weekdays
 *
 * Must be calender order.
 */
enum nsc_time_weekdays {
	NSC_TIME_WEEKDAY_SUN,
	NSC_TIME_WEEKDAY_MON,
	NSC_TIME_WEEKDAY_TUE,
	NSC_TIME_WEEKDAY_WED,
	NSC_TIME_WEEKDAY_THU,
	NSC_TIME_WEEKDAY_FRI,
	NSC_TIME_WEEKDAY_SAT,
	NSC_TIME_WEEKDAY__COUNT
};
/**
 * Months
 *
 * Must be calender order.
 */
enum nsc_time_months {
	NSC_TIME_MONTH_JAN,
	NSC_TIME_MONTH_FEB,
	NSC_TIME_MONTH_MAR,
	NSC_TIME_MONTH_APR,
	NSC_TIME_MONTH_MAY,
	NSC_TIME_MONTH_JUN,
	NSC_TIME_MONTH_JUL,
	NSC_TIME_MONTH_AUG,
	NSC_TIME_MONTH_SEP,
	NSC_TIME_MONTH_OCT,
	NSC_TIME_MONTH_NOV,
	NSC_TIME_MONTH_DEC,
	NSC_TIME_MONTH__COUNT,
};


/**
 * Array of short weekday names.
 */
static const char * const weekdays_short[NSC_TIME_WEEKDAY__COUNT] = {
	[NSC_TIME_WEEKDAY_SUN] = "Sun",
	[NSC_TIME_WEEKDAY_MON] = "Mon",
	[NSC_TIME_WEEKDAY_TUE] = "Tue",
	[NSC_TIME_WEEKDAY_WED] = "Wed",
	[NSC_TIME_WEEKDAY_THU] = "Thu",
	[NSC_TIME_WEEKDAY_FRI] = "Fri",
	[NSC_TIME_WEEKDAY_SAT] = "Sat"
};
/**
 * Array of month names.
 */
static const char * const months[NSC_TIME_MONTH__COUNT] = {
	[NSC_TIME_MONTH_JAN] = "Jan",
	[NSC_TIME_MONTH_FEB] = "Feb",
	[NSC_TIME_MONTH_MAR] = "Mar",
	[NSC_TIME_MONTH_APR] = "Apr",
	[NSC_TIME_MONTH_MAY] = "May",
	[NSC_TIME_MONTH_JUN] = "Jun",
	[NSC_TIME_MONTH_JUL] = "Jul",
	[NSC_TIME_MONTH_AUG] = "Aug",
	[NSC_TIME_MONTH_SEP] = "Sep",
	[NSC_TIME_MONTH_OCT] = "Oct",
	[NSC_TIME_MONTH_NOV] = "Nov",
	[NSC_TIME_MONTH_DEC] = "Dec"
};


/* exported interface documented in utils/time.h */
const char *rfc1123_date(time_t t)
{
	static char ret[31];

	struct tm *tm = gmtime(&t);

	snprintf(ret, sizeof ret, "%s, %02d %s %d %02d:%02d:%02d GMT",
			weekdays_short[tm->tm_wday], tm->tm_mday,
			months[tm->tm_mon], tm->tm_year + 1900,
			tm->tm_hour, tm->tm_min, tm->tm_sec);

	return ret;
}


/* exported function documented in utils/time.h */
int nsc_sntimet(char *str, size_t size, time_t *timep)
{
#ifndef HAVE_STRFTIME
	long long val;
	val = (long long)*timep;

	return snprintf(str, size, "%lld", val);
#else
	struct tm *ltm;

	ltm = localtime(timep);
	if (ltm == NULL) {
		return -1;
	}

	return strftime(str, size, "%s", ltm);
#endif
}


/* exported function documented in utils/time.h */
nserror nsc_snptimet(const char *str, size_t size, time_t *timep)
{
	time_t time_out;

#ifndef HAVE_STRPTIME
	char *rstr;

	if (size < 1) {
		return NSERROR_BAD_PARAMETER;
	}

	errno = 0;
	time_out = (time_t)strtoll(str, &rstr, 10);

	/* The conversion may have a range faliure or no digits were found */
	if ((errno != 0) || (rstr == str)) {
		return NSERROR_BAD_PARAMETER;
	}

#else
	struct tm ltm;

	if (size < 1) {
		return NSERROR_BAD_PARAMETER;
	}

	if (strptime(str, "%s", &ltm) == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	time_out = mktime(&ltm);

#endif
	*timep = time_out;

	return NSERROR_OK;
}


#ifndef WITH_CURL


/**
 * Array of long weekday names.
 */
static const char * const weekdays_long[NSC_TIME_WEEKDAY__COUNT] = {
	[NSC_TIME_WEEKDAY_SUN] = "Sunday",
	[NSC_TIME_WEEKDAY_MON] = "Monday",
	[NSC_TIME_WEEKDAY_TUE] = "Tuesday",
	[NSC_TIME_WEEKDAY_WED] = "Wednesday",
	[NSC_TIME_WEEKDAY_THU] = "Thursday",
	[NSC_TIME_WEEKDAY_FRI] = "Friday",
	[NSC_TIME_WEEKDAY_SAT] = "Saturday"
};

/**
 * Timezone offsets in mins
 *
 * Order doesn't matter.
 */
enum nsc_time_zone_offsets {
	/* Timezones */
	NSC_TIME_ZONE_OFFSET_IDLE = -12 * 60,
	NSC_TIME_ZONE_OFFSET_NZST = -12 * 60,
	NSC_TIME_ZONE_OFFSET_NZT  = -12 * 60,
	NSC_TIME_ZONE_OFFSET_EAST = -10 * 60,
	NSC_TIME_ZONE_OFFSET_GST  = -10 * 60,
	NSC_TIME_ZONE_OFFSET_JST  = - 9 * 60,
	NSC_TIME_ZONE_OFFSET_CCT  = - 8 * 60,
	NSC_TIME_ZONE_OFFSET_WAST = - 7 * 60,
	NSC_TIME_ZONE_OFFSET_EET  = - 2 * 60,
	NSC_TIME_ZONE_OFFSET_CET  = - 1 * 60,
	NSC_TIME_ZONE_OFFSET_FWT  = - 1 * 60,
	NSC_TIME_ZONE_OFFSET_MET  = - 1 * 60,
	NSC_TIME_ZONE_OFFSET_MEWT = - 1 * 60,
	NSC_TIME_ZONE_OFFSET_GMT  =   0,
	NSC_TIME_ZONE_OFFSET_UTC  =   0,
	NSC_TIME_ZONE_OFFSET_WET  =   0,
	NSC_TIME_ZONE_OFFSET_WAT  =   1 * 60,
	NSC_TIME_ZONE_OFFSET_AST  =   4 * 60,
	NSC_TIME_ZONE_OFFSET_EST  =   5 * 60,
	NSC_TIME_ZONE_OFFSET_CST  =   6 * 60,
	NSC_TIME_ZONE_OFFSET_MST  =   7 * 60,
	NSC_TIME_ZONE_OFFSET_PST  =   8 * 60,
	NSC_TIME_ZONE_OFFSET_YST  =   9 * 60,
	NSC_TIME_ZONE_OFFSET_AHST =  10 * 60,
	NSC_TIME_ZONE_OFFSET_CAT  =  10 * 60,
	NSC_TIME_ZONE_OFFSET_HST  =  10 * 60,
	NSC_TIME_ZONE_OFFSET_IDLW =  12 * 60,

	/* Daylight saving modified timezones */
	NSC_TIME_ZONE_OFFSET_NZDT = NSC_TIME_ZONE_OFFSET_NZT  - 60,
	NSC_TIME_ZONE_OFFSET_EADT = NSC_TIME_ZONE_OFFSET_EAST - 60,
	NSC_TIME_ZONE_OFFSET_WADT = NSC_TIME_ZONE_OFFSET_WAST - 60,
	NSC_TIME_ZONE_OFFSET_CEST = NSC_TIME_ZONE_OFFSET_CET  - 60,
	NSC_TIME_ZONE_OFFSET_FST  = NSC_TIME_ZONE_OFFSET_FWT  - 60,
	NSC_TIME_ZONE_OFFSET_MEST = NSC_TIME_ZONE_OFFSET_MET  - 60,
	NSC_TIME_ZONE_OFFSET_MESZ = NSC_TIME_ZONE_OFFSET_MET  - 60,
	NSC_TIME_ZONE_OFFSET_BST  = NSC_TIME_ZONE_OFFSET_GMT  - 60,
	NSC_TIME_ZONE_OFFSET_ADT  = NSC_TIME_ZONE_OFFSET_AST  - 60,
	NSC_TIME_ZONE_OFFSET_EDT  = NSC_TIME_ZONE_OFFSET_EST  - 60,
	NSC_TIME_ZONE_OFFSET_CDT  = NSC_TIME_ZONE_OFFSET_CST  - 60,
	NSC_TIME_ZONE_OFFSET_MDT  = NSC_TIME_ZONE_OFFSET_MST  - 60,
	NSC_TIME_ZONE_OFFSET_PDT  = NSC_TIME_ZONE_OFFSET_PST  - 60,
	NSC_TIME_ZONE_OFFSET_YDT  = NSC_TIME_ZONE_OFFSET_YST  - 60,
	NSC_TIME_ZONE_OFFSET_HDT  = NSC_TIME_ZONE_OFFSET_HST  - 60,

	/* Military timezones */
	NSC_TIME_ZONE_OFFSET_Y    = -12 * 60,
	NSC_TIME_ZONE_OFFSET_X    = -11 * 60,
	NSC_TIME_ZONE_OFFSET_W    = -10 * 60,
	NSC_TIME_ZONE_OFFSET_V    = - 9 * 60,
	NSC_TIME_ZONE_OFFSET_U    = - 8 * 60,
	NSC_TIME_ZONE_OFFSET_T    = - 7 * 60,
	NSC_TIME_ZONE_OFFSET_S    = - 6 * 60,
	NSC_TIME_ZONE_OFFSET_R    = - 5 * 60,
	NSC_TIME_ZONE_OFFSET_Q    = - 4 * 60,
	NSC_TIME_ZONE_OFFSET_P    = - 3 * 60,
	NSC_TIME_ZONE_OFFSET_O    = - 2 * 60,
	NSC_TIME_ZONE_OFFSET_N    = - 1 * 60,
	NSC_TIME_ZONE_OFFSET_Z    =   0 * 60,
	NSC_TIME_ZONE_OFFSET_A    =   1 * 60,
	NSC_TIME_ZONE_OFFSET_B    =   2 * 60,
	NSC_TIME_ZONE_OFFSET_C    =   3 * 60,
	NSC_TIME_ZONE_OFFSET_D    =   4 * 60,
	NSC_TIME_ZONE_OFFSET_E    =   5 * 60,
	NSC_TIME_ZONE_OFFSET_F    =   6 * 60,
	NSC_TIME_ZONE_OFFSET_G    =   7 * 60,
	NSC_TIME_ZONE_OFFSET_H    =   8 * 60,
	NSC_TIME_ZONE_OFFSET_I    =   9 * 60,
	NSC_TIME_ZONE_OFFSET_K    =  10 * 60,
	NSC_TIME_ZONE_OFFSET_L    =  11 * 60,
	NSC_TIME_ZONE_OFFSET_M    =  12 * 60,
};

/**
 * List of timezones.
 *
 * The order here is the order they appear in the `timezone_mins` and
 * `timezones` arrays.  So there is value in putting the most common
 * timezones first.
 */
enum nsc_time_zones {
	/** "GMT" first since its the only one I've seen in the wild. -- tlsa */
	NSC_TIME_ZONE_GMT,
	NSC_TIME_ZONE_IDLE,
	NSC_TIME_ZONE_NZST,
	NSC_TIME_ZONE_NZT,
	NSC_TIME_ZONE_EAST,
	NSC_TIME_ZONE_GST,
	NSC_TIME_ZONE_JST,
	NSC_TIME_ZONE_CCT,
	NSC_TIME_ZONE_WAST,
	NSC_TIME_ZONE_EET,
	NSC_TIME_ZONE_CET,
	NSC_TIME_ZONE_FWT,
	NSC_TIME_ZONE_MET,
	NSC_TIME_ZONE_MEWT,
	NSC_TIME_ZONE_UTC,
	NSC_TIME_ZONE_WET,
	NSC_TIME_ZONE_WAT,
	NSC_TIME_ZONE_AST,
	NSC_TIME_ZONE_EST,
	NSC_TIME_ZONE_CST,
	NSC_TIME_ZONE_MST,
	NSC_TIME_ZONE_PST,
	NSC_TIME_ZONE_YST,
	NSC_TIME_ZONE_AHST,
	NSC_TIME_ZONE_CAT,
	NSC_TIME_ZONE_HST,
	NSC_TIME_ZONE_IDLW,
	NSC_TIME_ZONE_NZDT,
	NSC_TIME_ZONE_EADT,
	NSC_TIME_ZONE_WADT,
	NSC_TIME_ZONE_CEST,
	NSC_TIME_ZONE_FST,
	NSC_TIME_ZONE_MEST,
	NSC_TIME_ZONE_MESZ,
	NSC_TIME_ZONE_BST,
	NSC_TIME_ZONE_ADT,
	NSC_TIME_ZONE_EDT,
	NSC_TIME_ZONE_CDT,
	NSC_TIME_ZONE_MDT,
	NSC_TIME_ZONE_PDT,
	NSC_TIME_ZONE_YDT,
	NSC_TIME_ZONE_HDT,
	NSC_TIME_ZONE_Y,
	NSC_TIME_ZONE_X,
	NSC_TIME_ZONE_W,
	NSC_TIME_ZONE_V,
	NSC_TIME_ZONE_U,
	NSC_TIME_ZONE_T,
	NSC_TIME_ZONE_S,
	NSC_TIME_ZONE_R,
	NSC_TIME_ZONE_Q,
	NSC_TIME_ZONE_P,
	NSC_TIME_ZONE_O,
	NSC_TIME_ZONE_N,
	NSC_TIME_ZONE_Z,
	NSC_TIME_ZONE_A,
	NSC_TIME_ZONE_B,
	NSC_TIME_ZONE_C,
	NSC_TIME_ZONE_D,
	NSC_TIME_ZONE_E,
	NSC_TIME_ZONE_F,
	NSC_TIME_ZONE_G,
	NSC_TIME_ZONE_H,
	NSC_TIME_ZONE_I,
	NSC_TIME_ZONE_K,
	NSC_TIME_ZONE_L,
	NSC_TIME_ZONE_M,
	NSC_TIME_ZONE__COUNT
};

/**
 * Array of minute offsets for timezones.
 */
static const int16_t timezone_mins[NSC_TIME_ZONE__COUNT] = {
	[NSC_TIME_ZONE_IDLE] = NSC_TIME_ZONE_OFFSET_IDLE,
	[NSC_TIME_ZONE_NZST] = NSC_TIME_ZONE_OFFSET_NZST,
	[NSC_TIME_ZONE_NZT]  = NSC_TIME_ZONE_OFFSET_NZT,
	[NSC_TIME_ZONE_EAST] = NSC_TIME_ZONE_OFFSET_EAST,
	[NSC_TIME_ZONE_GST]  = NSC_TIME_ZONE_OFFSET_GST,
	[NSC_TIME_ZONE_JST]  = NSC_TIME_ZONE_OFFSET_JST,
	[NSC_TIME_ZONE_CCT]  = NSC_TIME_ZONE_OFFSET_CCT,
	[NSC_TIME_ZONE_WAST] = NSC_TIME_ZONE_OFFSET_WAST,
	[NSC_TIME_ZONE_EET]  = NSC_TIME_ZONE_OFFSET_EET,
	[NSC_TIME_ZONE_CET]  = NSC_TIME_ZONE_OFFSET_CET,
	[NSC_TIME_ZONE_FWT]  = NSC_TIME_ZONE_OFFSET_FWT,
	[NSC_TIME_ZONE_MET]  = NSC_TIME_ZONE_OFFSET_MET,
	[NSC_TIME_ZONE_MEWT] = NSC_TIME_ZONE_OFFSET_MEWT,
	[NSC_TIME_ZONE_GMT]  = NSC_TIME_ZONE_OFFSET_GMT,
	[NSC_TIME_ZONE_UTC]  = NSC_TIME_ZONE_OFFSET_UTC,
	[NSC_TIME_ZONE_WET]  = NSC_TIME_ZONE_OFFSET_WET,
	[NSC_TIME_ZONE_WAT]  = NSC_TIME_ZONE_OFFSET_WAT,
	[NSC_TIME_ZONE_AST]  = NSC_TIME_ZONE_OFFSET_AST,
	[NSC_TIME_ZONE_EST]  = NSC_TIME_ZONE_OFFSET_EST,
	[NSC_TIME_ZONE_CST]  = NSC_TIME_ZONE_OFFSET_CST,
	[NSC_TIME_ZONE_MST]  = NSC_TIME_ZONE_OFFSET_MST,
	[NSC_TIME_ZONE_PST]  = NSC_TIME_ZONE_OFFSET_PST,
	[NSC_TIME_ZONE_YST]  = NSC_TIME_ZONE_OFFSET_YST,
	[NSC_TIME_ZONE_AHST] = NSC_TIME_ZONE_OFFSET_AHST,
	[NSC_TIME_ZONE_CAT]  = NSC_TIME_ZONE_OFFSET_CAT,
	[NSC_TIME_ZONE_HST]  = NSC_TIME_ZONE_OFFSET_HST,
	[NSC_TIME_ZONE_IDLW] = NSC_TIME_ZONE_OFFSET_IDLW,
	[NSC_TIME_ZONE_NZDT] = NSC_TIME_ZONE_OFFSET_NZDT,
	[NSC_TIME_ZONE_EADT] = NSC_TIME_ZONE_OFFSET_EADT,
	[NSC_TIME_ZONE_WADT] = NSC_TIME_ZONE_OFFSET_WADT,
	[NSC_TIME_ZONE_CEST] = NSC_TIME_ZONE_OFFSET_CEST,
	[NSC_TIME_ZONE_FST]  = NSC_TIME_ZONE_OFFSET_FST,
	[NSC_TIME_ZONE_MEST] = NSC_TIME_ZONE_OFFSET_MEST,
	[NSC_TIME_ZONE_MESZ] = NSC_TIME_ZONE_OFFSET_MESZ,
	[NSC_TIME_ZONE_BST]  = NSC_TIME_ZONE_OFFSET_BST,
	[NSC_TIME_ZONE_ADT]  = NSC_TIME_ZONE_OFFSET_ADT,
	[NSC_TIME_ZONE_EDT]  = NSC_TIME_ZONE_OFFSET_EDT,
	[NSC_TIME_ZONE_CDT]  = NSC_TIME_ZONE_OFFSET_CDT,
	[NSC_TIME_ZONE_MDT]  = NSC_TIME_ZONE_OFFSET_MDT,
	[NSC_TIME_ZONE_PDT]  = NSC_TIME_ZONE_OFFSET_PDT,
	[NSC_TIME_ZONE_YDT]  = NSC_TIME_ZONE_OFFSET_YDT,
	[NSC_TIME_ZONE_HDT]  = NSC_TIME_ZONE_OFFSET_HDT,
	[NSC_TIME_ZONE_Y]    = NSC_TIME_ZONE_OFFSET_Y,
	[NSC_TIME_ZONE_X]    = NSC_TIME_ZONE_OFFSET_X,
	[NSC_TIME_ZONE_W]    = NSC_TIME_ZONE_OFFSET_W,
	[NSC_TIME_ZONE_V]    = NSC_TIME_ZONE_OFFSET_V,
	[NSC_TIME_ZONE_U]    = NSC_TIME_ZONE_OFFSET_U,
	[NSC_TIME_ZONE_T]    = NSC_TIME_ZONE_OFFSET_T,
	[NSC_TIME_ZONE_S]    = NSC_TIME_ZONE_OFFSET_S,
	[NSC_TIME_ZONE_R]    = NSC_TIME_ZONE_OFFSET_R,
	[NSC_TIME_ZONE_Q]    = NSC_TIME_ZONE_OFFSET_Q,
	[NSC_TIME_ZONE_P]    = NSC_TIME_ZONE_OFFSET_P,
	[NSC_TIME_ZONE_O]    = NSC_TIME_ZONE_OFFSET_O,
	[NSC_TIME_ZONE_N]    = NSC_TIME_ZONE_OFFSET_N,
	[NSC_TIME_ZONE_Z]    = NSC_TIME_ZONE_OFFSET_Z,
	[NSC_TIME_ZONE_A]    = NSC_TIME_ZONE_OFFSET_A,
	[NSC_TIME_ZONE_B]    = NSC_TIME_ZONE_OFFSET_B,
	[NSC_TIME_ZONE_C]    = NSC_TIME_ZONE_OFFSET_C,
	[NSC_TIME_ZONE_D]    = NSC_TIME_ZONE_OFFSET_D,
	[NSC_TIME_ZONE_E]    = NSC_TIME_ZONE_OFFSET_E,
	[NSC_TIME_ZONE_F]    = NSC_TIME_ZONE_OFFSET_F,
	[NSC_TIME_ZONE_G]    = NSC_TIME_ZONE_OFFSET_G,
	[NSC_TIME_ZONE_H]    = NSC_TIME_ZONE_OFFSET_H,
	[NSC_TIME_ZONE_I]    = NSC_TIME_ZONE_OFFSET_I,
	[NSC_TIME_ZONE_K]    = NSC_TIME_ZONE_OFFSET_K,
	[NSC_TIME_ZONE_L]    = NSC_TIME_ZONE_OFFSET_L,
	[NSC_TIME_ZONE_M]    = NSC_TIME_ZONE_OFFSET_M
};

/**
 * Array of timezone names.  Order does not matter.
 */
static const char * const timezones[NSC_TIME_ZONE__COUNT] = {
	[NSC_TIME_ZONE_IDLE] = "IDLE",
	[NSC_TIME_ZONE_NZST] = "NZST",
	[NSC_TIME_ZONE_NZT]  = "NZT",
	[NSC_TIME_ZONE_EAST] = "EAST",
	[NSC_TIME_ZONE_GST]  = "GST",
	[NSC_TIME_ZONE_JST]  = "JST",
	[NSC_TIME_ZONE_CCT]  = "CCT",
	[NSC_TIME_ZONE_WAST] = "WAST",
	[NSC_TIME_ZONE_EET]  = "EET",
	[NSC_TIME_ZONE_CET]  = "CET",
	[NSC_TIME_ZONE_FWT]  = "FWT",
	[NSC_TIME_ZONE_MET]  = "MET",
	[NSC_TIME_ZONE_MEWT] = "MEWT",
	[NSC_TIME_ZONE_GMT]  = "GMT",
	[NSC_TIME_ZONE_UTC]  = "UTC",
	[NSC_TIME_ZONE_WET]  = "WET",
	[NSC_TIME_ZONE_WAT]  = "WAT",
	[NSC_TIME_ZONE_AST]  = "AST",
	[NSC_TIME_ZONE_EST]  = "EST",
	[NSC_TIME_ZONE_CST]  = "CST",
	[NSC_TIME_ZONE_MST]  = "MST",
	[NSC_TIME_ZONE_PST]  = "PST",
	[NSC_TIME_ZONE_YST]  = "YST",
	[NSC_TIME_ZONE_AHST] = "AHST",
	[NSC_TIME_ZONE_CAT]  = "CAT",
	[NSC_TIME_ZONE_HST]  = "HST",
	[NSC_TIME_ZONE_IDLW] = "IDLW",
	[NSC_TIME_ZONE_NZDT] = "NZDT",
	[NSC_TIME_ZONE_EADT] = "EADT",
	[NSC_TIME_ZONE_WADT] = "WADT",
	[NSC_TIME_ZONE_CEST] = "CEST",
	[NSC_TIME_ZONE_FST]  = "FST",
	[NSC_TIME_ZONE_MEST] = "MEST",
	[NSC_TIME_ZONE_MESZ] = "MESZ",
	[NSC_TIME_ZONE_BST]  = "BST",
	[NSC_TIME_ZONE_ADT]  = "ADT",
	[NSC_TIME_ZONE_EDT]  = "EDT",
	[NSC_TIME_ZONE_CDT]  = "CDT",
	[NSC_TIME_ZONE_MDT]  = "MDT",
	[NSC_TIME_ZONE_PDT]  = "PDT",
	[NSC_TIME_ZONE_YDT]  = "YDT",
	[NSC_TIME_ZONE_HDT]  = "HDT",
	[NSC_TIME_ZONE_Y]    = "Y",
	[NSC_TIME_ZONE_X]    = "X",
	[NSC_TIME_ZONE_W]    = "W",
	[NSC_TIME_ZONE_V]    = "V",
	[NSC_TIME_ZONE_U]    = "U",
	[NSC_TIME_ZONE_T]    = "T",
	[NSC_TIME_ZONE_S]    = "S",
	[NSC_TIME_ZONE_R]    = "R",
	[NSC_TIME_ZONE_Q]    = "Q",
	[NSC_TIME_ZONE_P]    = "P",
	[NSC_TIME_ZONE_O]    = "O",
	[NSC_TIME_ZONE_N]    = "N",
	[NSC_TIME_ZONE_Z]    = "Z",
	[NSC_TIME_ZONE_A]    = "A",
	[NSC_TIME_ZONE_B]    = "B",
	[NSC_TIME_ZONE_C]    = "C",
	[NSC_TIME_ZONE_D]    = "D",
	[NSC_TIME_ZONE_E]    = "E",
	[NSC_TIME_ZONE_F]    = "F",
	[NSC_TIME_ZONE_G]    = "G",
	[NSC_TIME_ZONE_H]    = "H",
	[NSC_TIME_ZONE_I]    = "I",
	[NSC_TIME_ZONE_K]    = "K",
	[NSC_TIME_ZONE_L]    = "L",
	[NSC_TIME_ZONE_M]    = "M"
};

/**
 * Flags for tracking the components of a date that have been parsed.
 */
enum nsc_date_component_flags {
	NSC_COMPONENT_FLAGS_NONE            = 0,
	NSC_COMPONENT_FLAGS_HAVE_YEARS      = (1 << 0),
	NSC_COMPONENT_FLAGS_HAVE_MONTHS     = (1 << 1),
	NSC_COMPONENT_FLAGS_HAVE_DAYS       = (1 << 2),
	NSC_COMPONENT_FLAGS_HAVE_HOURS      = (1 << 3),
	NSC_COMPONENT_FLAGS_HAVE_MINS       = (1 << 4),
	NSC_COMPONENT_FLAGS_HAVE_SECS       = (1 << 5),
	NSC_COMPONENT_FLAGS_HAVE_TIMEZONE   = (1 << 6),
	NSC_COMPONENT_FLAGS_HAVE_WEEKDAY    = (1 << 7),
	NSC_COMPONENT_FLAGS__HAVE_YYYYMMDD  =
			NSC_COMPONENT_FLAGS_HAVE_YEARS   |
			NSC_COMPONENT_FLAGS_HAVE_MONTHS  |
			NSC_COMPONENT_FLAGS_HAVE_DAYS,
	NSC_COMPONENT_FLAGS__HAVE_HHMMSS    =
			NSC_COMPONENT_FLAGS_HAVE_HOURS   |
			NSC_COMPONENT_FLAGS_HAVE_MINS    |
			NSC_COMPONENT_FLAGS_HAVE_SECS,
	NSC_COMPONENT_FLAGS__HAVE_ALL       =
			NSC_COMPONENT_FLAGS_HAVE_YEARS   |
			NSC_COMPONENT_FLAGS_HAVE_MONTHS  |
			NSC_COMPONENT_FLAGS_HAVE_DAYS    |
			NSC_COMPONENT_FLAGS_HAVE_HOURS   |
			NSC_COMPONENT_FLAGS_HAVE_MINS    |
			NSC_COMPONENT_FLAGS_HAVE_SECS    |
			NSC_COMPONENT_FLAGS_HAVE_TIMEZONE
};

/**
 * Context for date parsing.
 */
struct nsc_date_parse_ctx {
	char prev; /**< Used for handling neumenrical timezone */
	uint8_t secs;
	uint8_t mins;
	uint8_t hours;
	uint8_t day;
	uint8_t month;
	uint16_t years;
	int16_t timezone_offset_mins;
};


/**
 * Helper for testing whether any of the flags in mask are set.
 *
 * \param[in] flags  Flags to to check.
 * \param[in] mask   The set of flags to check for in `flags`.
 * \return true iff any flags in `mask` are set in `flags`, else false.
 */
static bool flags_chk(
		enum nsc_date_component_flags flags,
		enum nsc_date_component_flags mask)
{
	return flags & mask;
}

/**
 * Helper for testing whether all of the flags in mask are set.
 *
 * \param[in] flags  Flags to to check.
 * \param[in] mask   The set of flags to check for in `flags`.
 * \return true iff all flags in `mask` are set in `flags`, else false.
 */
static bool flags_chk_all(
		enum nsc_date_component_flags flags,
		enum nsc_date_component_flags mask)
{
	return (flags & mask) == mask;
}


/**
 * Test for a weekday name in a string (case insensitive).
 *
 * \param[in]     str    String to parse a weekday name in.
 * \param[in]     len    Number of consecutive alphabetical characters.
 * \param[in,out] flags  Flags indicating which date components have been
 *                       found in `str` already.  If a weekday component
 *                       is found, the weekday flag is set.
 * \return true iff weekday component is found, else false.
 */
static inline bool time__parse_weekday(
		const char *str,
		size_t len,
		enum nsc_date_component_flags *flags)
{
	const char * const *names = (len == 3) ?
			weekdays_short : weekdays_long;

	if (flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_WEEKDAY)) {
		return false;
	}

	{
		uint32_t i;
		for (i = 0; i < NSC_TIME_WEEKDAY__COUNT; i++) {
			if (ascii_strings_count_equal_caseless(names[i], str) == len) {
				*flags |= NSC_COMPONENT_FLAGS_HAVE_WEEKDAY;
				return true;
			}
		}
	}

	return false;
}


/**
 * Attempt to parse a month name in a string (case insensitive).
 *
 * \param[in]     str    String to parse a month name in.
 * \param[in]     len    Number of consecutive alphabetical characters.
 * \param[in,out] flags  Flags indicating which date components have been
 *                       found in `str` already.  If a month component
 *                       is found, the month flag is set.
 * \param[in,out] ctx    Current date parsing context.  If month component
 *                       is found, the context's month value is set.
 * \return true iff month name component is found, else false.
 */
static inline bool time__parse_month(
		const char *str,
		size_t len,
		enum nsc_date_component_flags *flags,
		struct nsc_date_parse_ctx *ctx)
{
	if (flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_MONTHS)) {
		return false;
	}

	{
		uint32_t i;
		for (i = 0; i < NSC_TIME_MONTH__COUNT; i++) {
			if (ascii_strings_count_equal_caseless(months[i], str) == len) {
				*flags |= NSC_COMPONENT_FLAGS_HAVE_MONTHS;
				ctx->month = i;
				return true;
			}
		}
	}

	return false;
}


/**
 * Attempt to parse a timezone name in a string (case insensitive).
 *
 * \param[in]     str    String to parse a timezone name in.
 * \param[in]     len    Number of consecutive alphabetical characters.
 * \param[in,out] flags  Flags indicating which date components have been
 *                       found in `str` already.  If a timezone component
 *                       is found, the timezone flag is set.
 * \param[in,out] ctx    Current date parsing context.  If timezone component
 *                       is found, the context's timezone value is set.
 * \return true iff timezone name component is found, else false.
 */
static inline bool time__parse_timezone(
		const char *str,
		size_t len,
		enum nsc_date_component_flags *flags,
		struct nsc_date_parse_ctx *ctx)
{
	if (flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_TIMEZONE)) {
		return false;
	}

	{
		uint32_t i;
		for (i = 0; i < NSC_TIME_ZONE__COUNT; i++) {
			if (ascii_strings_count_equal_caseless(
					timezones[i], str) == len) {
				*flags |= NSC_COMPONENT_FLAGS_HAVE_TIMEZONE;
				ctx->timezone_offset_mins = timezone_mins[i];
				return true;
			}
		}
	}

	return false;
}


/**
 * Attempt to parse an "hh:mm:ss" time from a string.
 *
 * \param[in]     str    String to parse a time in.
 * \param[in,out] len    The number of characters until the first non-digit.
 *                       Iff a time component is found, updated to the number
 *                       of comsumend characters.
 * \param[in,out] flags  Flags indicating which date components have been
 *                       found in `str` already.  If a time component
 *                       is found, the hours, mins and secs flags are set.
 * \param[in,out] ctx    Current date parsing context.  If time component
 *                       is found, the context's time values are set.
 * \return true iff time component is found, else false.
 */
static inline bool time__parse_hh_mm_ss(
		const char *str,
		size_t *len,
		enum nsc_date_component_flags *flags,
		struct nsc_date_parse_ctx *ctx)
{
	size_t l;

	if (*len != 2 || flags_chk(*flags, NSC_COMPONENT_FLAGS__HAVE_HHMMSS)) {
		return false;
	}

	l = *len + ascii_count_digit_or_colon(str + *len);
	if (l == 8) {
		int h, m, s, count;
		count = sscanf(str, "%02d:%02d:%02d", &h, &m, &s);
		if (count == 3) {
			ctx->hours = h;
			ctx->mins  = m;
			ctx->secs  = s;
			*flags |= NSC_COMPONENT_FLAGS__HAVE_HHMMSS;
			*len = l;
			return true;
		}
	} else if (l == 5) {
		int h, m, count;
		count = sscanf(str, "%02d:%02d", &h, &m);
		if (count == 2) {
			ctx->hours = h;
			ctx->mins  = m;
			ctx->secs  = 0;
			*flags |= NSC_COMPONENT_FLAGS__HAVE_HHMMSS;
			*len = l;
			return true;
		}
	}

	return false;
}


/**
 * Attempt to parse a number from a date string.
 *
 * How the number is treated depends on various things:
 *
 * - its character length,
 * - its value,
 * - which date components have already been parsed
 *
 * \param[in]     str    String to parse a time in.
 * \param[in]     len    The number of characters until the first non-digit.
 * \param[in,out] flags  Flags indicating which date components have been
 *                       found in `str` already.  If any component is found,
 *                       their flags are set.
 * \param[in,out] ctx    Current date parsing context.  If any component
 *                       is found, the appropriate context values are set.
 * \return true iff a component is found, else false.
 */
static inline bool time__parse_number(
		const char *str,
		size_t len,
		enum nsc_date_component_flags *flags,
		struct nsc_date_parse_ctx *ctx)
{
	int value;

	if (len != ascii_string_to_int(str, &value)) {
		return false;
	}

	switch (len) {
	case 8:
		if (!flags_chk(*flags, NSC_COMPONENT_FLAGS__HAVE_YYYYMMDD)) {
			ctx->years  =  value / 10000;
			ctx->month = (value % 10000) / 100 - 1;
			ctx->day   =  value % 100 - 1;
			*flags |= NSC_COMPONENT_FLAGS__HAVE_YYYYMMDD;
			return true;
		}
		break;

	case 4:
		if (!flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_TIMEZONE)) {
			if (ascii_is_sign(ctx->prev) && value <= 1400) {
				ctx->timezone_offset_mins =
						value / 100 * 60 +
						value % 100;
				if (ctx->prev == '+') {
					ctx->timezone_offset_mins *= -1;
				}
				*flags |= NSC_COMPONENT_FLAGS_HAVE_TIMEZONE;
				return true;
			}
		}
		if (!flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_YEARS)) {
			ctx->years = value;
			*flags |= NSC_COMPONENT_FLAGS_HAVE_YEARS;
			return true;
		}
		break;

	case 2:
	case 1:
		if (!flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_DAYS) &&
				value > 0 && value <= 31) {
			ctx->day = value - 1;
			*flags |= NSC_COMPONENT_FLAGS_HAVE_DAYS;
			return true;
		}
		if (!flags_chk(*flags, NSC_COMPONENT_FLAGS_HAVE_YEARS)) {
			ctx->years = (value > 70) ?
					value + 1900 :
					value + 2000;
			*flags |= NSC_COMPONENT_FLAGS_HAVE_YEARS;
			return true;
		}
		break;

	default:
		break;
	}

	return false;
}

/**
 * Get number of leap days up until end of given year.
 *
 * \param[in] year  Year to count leap years up to.
 * \return Number of leap days up to end of `year`.
 */
static inline int time__get_leap_days(int year)
{
	return (year / 4) - (year / 100) + (year / 400);
}


/**
 * Helper to convert a date string context to a time_t.
 *
 * \param[in]  ctx    Current date parsing context.
 * \param[in]  flags  Flags indicating which date components have been set.
 * \param[out] time   Returns the number of seconds since 1 Jan 1970 00:00 UTC.
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
static nserror time__ctx_to_time_t(
		const struct nsc_date_parse_ctx *ctx,
		enum nsc_date_component_flags flags,
		time_t *time)
{
	enum {
		NSC_MONTH_DAYS_JAN = 31,
		NSC_MONTH_DAYS_FEB = 28, /**< Leap years handled separatly */
		NSC_MONTH_DAYS_MAR = 31,
		NSC_MONTH_DAYS_APR = 30,
		NSC_MONTH_DAYS_MAY = 31,
		NSC_MONTH_DAYS_JUN = 30,
		NSC_MONTH_DAYS_JUL = 31,
		NSC_MONTH_DAYS_AUG = 31,
		NSC_MONTH_DAYS_SEP = 30,
		NSC_MONTH_DAYS_OCT = 31,
		NSC_MONTH_DAYS_NOV = 30,
		NSC_MONTH_DAYS_DEC = 31
	};
	enum {
		NSC_MONTH_OFF_JAN = 0,
		NSC_MONTH_OFF_FEB = NSC_MONTH_OFF_JAN + NSC_MONTH_DAYS_JAN,
		NSC_MONTH_OFF_MAR = NSC_MONTH_OFF_FEB + NSC_MONTH_DAYS_FEB,
		NSC_MONTH_OFF_APR = NSC_MONTH_OFF_MAR + NSC_MONTH_DAYS_MAR,
		NSC_MONTH_OFF_MAY = NSC_MONTH_OFF_APR + NSC_MONTH_DAYS_APR,
		NSC_MONTH_OFF_JUN = NSC_MONTH_OFF_MAY + NSC_MONTH_DAYS_MAY,
		NSC_MONTH_OFF_JUL = NSC_MONTH_OFF_JUN + NSC_MONTH_DAYS_JUN,
		NSC_MONTH_OFF_AUG = NSC_MONTH_OFF_JUL + NSC_MONTH_DAYS_JUL,
		NSC_MONTH_OFF_SEP = NSC_MONTH_OFF_AUG + NSC_MONTH_DAYS_AUG,
		NSC_MONTH_OFF_OCT = NSC_MONTH_OFF_SEP + NSC_MONTH_DAYS_SEP,
		NSC_MONTH_OFF_NOV = NSC_MONTH_OFF_OCT + NSC_MONTH_DAYS_OCT,
		NSC_MONTH_OFF_DEC = NSC_MONTH_OFF_NOV + NSC_MONTH_DAYS_NOV
	};
	static const uint16_t month_offsets[NSC_TIME_MONTH__COUNT] = {
		[NSC_TIME_MONTH_JAN] = NSC_MONTH_OFF_JAN,
		[NSC_TIME_MONTH_FEB] = NSC_MONTH_OFF_FEB,
		[NSC_TIME_MONTH_MAR] = NSC_MONTH_OFF_MAR,
		[NSC_TIME_MONTH_APR] = NSC_MONTH_OFF_APR,
		[NSC_TIME_MONTH_MAY] = NSC_MONTH_OFF_MAY,
		[NSC_TIME_MONTH_JUN] = NSC_MONTH_OFF_JUN,
		[NSC_TIME_MONTH_JUL] = NSC_MONTH_OFF_JUL,
		[NSC_TIME_MONTH_AUG] = NSC_MONTH_OFF_AUG,
		[NSC_TIME_MONTH_SEP] = NSC_MONTH_OFF_SEP,
		[NSC_TIME_MONTH_OCT] = NSC_MONTH_OFF_OCT,
		[NSC_TIME_MONTH_NOV] = NSC_MONTH_OFF_NOV,
		[NSC_TIME_MONTH_DEC] = NSC_MONTH_OFF_DEC
	};
	int year_days = (ctx->years - 1970) * 365;
	int month_days = month_offsets[ctx->month];
	int year = (ctx->month < NSC_TIME_MONTH_FEB) ?
			ctx->years - 1 : ctx->years;
	int leap_days = time__get_leap_days(year) - time__get_leap_days(1969);
	int total_days = year_days + month_days + ctx->day + leap_days;

	int mins = (int)ctx->mins + (int)ctx->timezone_offset_mins;

	*time = (((((time_t)(total_days)) * 24) +
			ctx->hours) * 60 +
			mins) * 60 +
			ctx->secs;
	return NSERROR_OK;
}


/**
 * Parse a date string to a `time_t`.
 *
 * \param[in]  str   String to parse.
 * \param[out] time  Returns the number of seconds since 1 Jan 1970 00:00 UTC.
 * \return `NSERROR_OK` on success, else
 *         `NSERROR_INVALID` if the string parsing failed,
 *         appropriate error otherwise.
 */
static nserror time__get_date(const char *str, time_t *time)
{
	enum nsc_date_component_flags flags = NSC_COMPONENT_FLAGS_NONE;
	struct nsc_date_parse_ctx ctx = {
		.prev  = '\0',
		.secs  = 0,
		.mins  = 0,
		.hours = 0,
		.day   = 0,
		.month = 0,
		.years = 0,
		.timezone_offset_mins = 0
	};

	if (str == NULL || time == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* Parse */
	while (*str != '\0' &&
			!flags_chk_all(flags, NSC_COMPONENT_FLAGS__HAVE_ALL)) {
		size_t len = 1;

		if (ascii_is_alpha(*str)) {
			len += ascii_count_alpha(str + 1);

			if (!time__parse_weekday(str, len, &flags) &&
			    !time__parse_month(str, len, &flags, &ctx) &&
			    !time__parse_timezone(str, len, &flags, &ctx)) {
				return NSERROR_INVALID;
			}

		} else if (ascii_is_digit(*str)) {
			len += ascii_count_digit(str + 1);

			if (!time__parse_hh_mm_ss(str, &len, &flags, &ctx) &&
			    !time__parse_number(str, len, &flags, &ctx)) {
				return NSERROR_INVALID;
			}
		}
		ctx.prev = *str;
		str += len;
	}

	/* The initial values of 0 are used if hours, mins, secs, and timezone
	 * are not found */
	flags |= NSC_COMPONENT_FLAGS__HAVE_HHMMSS;
	flags |= NSC_COMPONENT_FLAGS_HAVE_TIMEZONE;

	/* Validate */
	if (!flags_chk_all(flags, NSC_COMPONENT_FLAGS__HAVE_ALL)) {
		return NSERROR_INVALID;
	}
	if (ctx.secs > 60 || ctx.mins > 59 || ctx.hours > 23 ||
			ctx.day > 31 || ctx.month > 11) {
		return NSERROR_INVALID;
	}

	/* Convert */
	return time__ctx_to_time_t(&ctx, flags, time);
}

/* exported function documented in utils/time.h */
nserror nsc_strntimet(const char *str, size_t size, time_t *timep)
{
	return time__get_date(str, timep);
}

# else

/* exported function documented in utils/time.h */
nserror nsc_strntimet(const char *str, size_t size, time_t *timep)
{
	time_t result;

	if (str == NULL || timep == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	result = curl_getdate(str, NULL);

	if (result == -1) {
		return NSERROR_INVALID;
	}

	*timep = result;

	return NSERROR_OK;
}

#endif
