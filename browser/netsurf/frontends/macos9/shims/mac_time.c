/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_time.c — POSIX time function shim using Mac OS 9 DateTimeUtils
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include "mac_time.h"
#include <string.h>

#ifdef __MACOS9__
#include <DateTimeUtils.h>
#include <Timer.h>
#include <Script.h>
#endif

#define MAC_EPOCH_OFFSET_I 2082844800L

static struct mac_tm tm_buf;

static const char *day_names_full[] = {
	"Sunday", "Monday", "Tuesday", "Wednesday",
	"Thursday", "Friday", "Saturday"
};

static const char *day_names_abbr[] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *month_names_full[] = {
	"January", "February", "March", "April",
	"May", "June", "July", "August",
	"September", "October", "November", "December"
};

static const char *month_names_abbr[] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Days in each month (non-leap) */
static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int is_leap(int year)
{
	return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int day_of_year(int year, int mon, int mday)
{
	int i, yday = 0;
	for (i = 0; i < mon; i++) {
		yday += mdays[i];
		if (i == 1 && is_leap(year))
			yday++;
	}
	return yday + mday - 1;
}

#ifdef __MACOS9__

static void dtrec_to_tm(const DateTimeRec *dt, struct mac_tm *out)
{
	out->tm_sec  = dt->second;
	out->tm_min  = dt->minute;
	out->tm_hour = dt->hour;
	out->tm_mday = dt->day;
	out->tm_mon  = dt->month - 1;
	out->tm_year = dt->year - 1900;
	out->tm_wday = dt->dayOfWeek - 1; /* Mac: 1=Sun, POSIX: 0=Sun */
	out->tm_yday = day_of_year(dt->year, dt->month - 1, dt->day);
	out->tm_isdst = -1;
}

int mac_gettimeofday(struct mac_timeval *tv, void *tz)
{
	unsigned long secs;
	UnsignedWide usecs;

	(void)tz;

	if (tv == NULL)
		return -1;

	GetDateTime(&secs);
	Microseconds(&usecs);

	tv->tv_sec = (long)secs - MAC_EPOCH_OFFSET_I;
	tv->tv_usec = (long)(usecs.lo % 1000000UL);

	return 0;
}

long mac_time(long *t)
{
	unsigned long secs;
	long result;

	GetDateTime(&secs);
	result = (long)secs - MAC_EPOCH_OFFSET_I;

	if (t != NULL)
		*t = result;

	return result;
}

struct mac_tm *mac_localtime(const long *timep)
{
	unsigned long mac_secs;
	DateTimeRec dt;

	if (timep == NULL)
		return NULL;

	mac_secs = (unsigned long)(*timep + MAC_EPOCH_OFFSET_I);
	SecondsToDate(mac_secs, &dt);
	dtrec_to_tm(&dt, &tm_buf);

	return &tm_buf;
}

struct mac_tm *mac_gmtime(const long *timep)
{
	MachineLocation loc;
	long gmt_delta;
	unsigned long mac_secs;
	DateTimeRec dt;

	if (timep == NULL)
		return NULL;

	/* Get GMT offset from system location */
	ReadLocation(&loc);
	gmt_delta = (long)loc.u.gmtDelta;
	/* gmtDelta is a 24-bit signed value in the low 3 bytes */
	if (gmt_delta & 0x00800000)
		gmt_delta |= 0xFF000000; /* sign extend */
	gmt_delta &= 0x00FFFFFF;

	/* Convert to Mac epoch, subtract local offset to get UTC */
	mac_secs = (unsigned long)(*timep + MAC_EPOCH_OFFSET_I);
	mac_secs -= (unsigned long)gmt_delta;

	SecondsToDate(mac_secs, &dt);
	dtrec_to_tm(&dt, &tm_buf);

	return &tm_buf;
}

long mac_mktime(struct mac_tm *tm)
{
	DateTimeRec dt;
	unsigned long secs;

	if (tm == NULL)
		return -1;

	dt.year   = (short)(tm->tm_year + 1900);
	dt.month  = (short)(tm->tm_mon + 1);
	dt.day    = (short)tm->tm_mday;
	dt.hour   = (short)tm->tm_hour;
	dt.minute = (short)tm->tm_min;
	dt.second = (short)tm->tm_sec;
	dt.dayOfWeek = 0; /* ignored by DateToSeconds */

	DateToSeconds(&dt, &secs);

	return (long)secs - MAC_EPOCH_OFFSET_I;
}

#else /* Linux stubs */

int mac_gettimeofday(struct mac_timeval *tv, void *tz)
{
	(void)tz;
	if (tv != NULL) {
		tv->tv_sec = 0;
		tv->tv_usec = 0;
	}
	return 0;
}

long mac_time(long *t)
{
	if (t != NULL)
		*t = 0;
	return 0;
}

struct mac_tm *mac_localtime(const long *timep)
{
	(void)timep;
	memset(&tm_buf, 0, sizeof(tm_buf));
	return &tm_buf;
}

struct mac_tm *mac_gmtime(const long *timep)
{
	(void)timep;
	memset(&tm_buf, 0, sizeof(tm_buf));
	return &tm_buf;
}

long mac_mktime(struct mac_tm *tm)
{
	(void)tm;
	return 0;
}

#endif /* __MACOS9__ */

/*
 * mac_strftime — manual format string implementation
 * Supports: %Y %m %d %H %M %S %A %B %a %b
 * Shared between Mac OS 9 and Linux (pure C, no Toolbox calls).
 */
size_t mac_strftime(char *s, size_t max, const char *fmt,
		    const struct mac_tm *tm)
{
	size_t pos = 0;
	const char *p;
	char num[8];
	const char *str;
	size_t len;
	int val;

	if (s == NULL || max == 0 || fmt == NULL || tm == NULL)
		return 0;

	for (p = fmt; *p != '\0' && pos < max - 1; p++) {
		if (*p != '%') {
			s[pos++] = *p;
			continue;
		}

		p++;
		if (*p == '\0')
			break;

		str = NULL;

		switch (*p) {
		case 'Y': /* 4-digit year */
			val = tm->tm_year + 1900;
			num[0] = (char)('0' + (val / 1000) % 10);
			num[1] = (char)('0' + (val / 100) % 10);
			num[2] = (char)('0' + (val / 10) % 10);
			num[3] = (char)('0' + val % 10);
			num[4] = '\0';
			str = num;
			break;
		case 'm': /* month 01-12 */
			val = tm->tm_mon + 1;
			num[0] = (char)('0' + val / 10);
			num[1] = (char)('0' + val % 10);
			num[2] = '\0';
			str = num;
			break;
		case 'd': /* day 01-31 */
			num[0] = (char)('0' + tm->tm_mday / 10);
			num[1] = (char)('0' + tm->tm_mday % 10);
			num[2] = '\0';
			str = num;
			break;
		case 'H': /* hour 00-23 */
			num[0] = (char)('0' + tm->tm_hour / 10);
			num[1] = (char)('0' + tm->tm_hour % 10);
			num[2] = '\0';
			str = num;
			break;
		case 'M': /* minute 00-59 */
			num[0] = (char)('0' + tm->tm_min / 10);
			num[1] = (char)('0' + tm->tm_min % 10);
			num[2] = '\0';
			str = num;
			break;
		case 'S': /* second 00-59 */
			num[0] = (char)('0' + tm->tm_sec / 10);
			num[1] = (char)('0' + tm->tm_sec % 10);
			num[2] = '\0';
			str = num;
			break;
		case 'A': /* full weekday name */
			if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
				str = day_names_full[tm->tm_wday];
			break;
		case 'a': /* abbreviated weekday */
			if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
				str = day_names_abbr[tm->tm_wday];
			break;
		case 'B': /* full month name */
			if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
				str = month_names_full[tm->tm_mon];
			break;
		case 'b': /* abbreviated month */
			if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
				str = month_names_abbr[tm->tm_mon];
			break;
		case '%':
			s[pos++] = '%';
			continue;
		default:
			/* Unknown specifier — emit literal */
			s[pos++] = '%';
			if (pos < max - 1)
				s[pos++] = *p;
			continue;
		}

		if (str != NULL) {
			len = strlen(str);
			if (pos + len >= max)
				len = max - 1 - pos;
			memcpy(s + pos, str, len);
			pos += len;
		}
	}

	s[pos] = '\0';
	return pos;
}

/*
 * gettimeofday — POSIX wrapper expected by NetSurf core.
 * Delegates to mac_gettimeofday internally.
 */
struct timeval;

int gettimeofday(struct timeval *tv, void *tz)
{
	struct mac_timeval mtv;
	int ret;

	ret = mac_gettimeofday(&mtv, tz);
	if (tv != NULL) {
		((long *)tv)[0] = mtv.tv_sec;
		((long *)tv)[1] = mtv.tv_usec;
	}
	return ret;
}
