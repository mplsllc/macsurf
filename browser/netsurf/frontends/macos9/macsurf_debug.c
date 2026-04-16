/*
 * MacSurf — macsurf_debug.c
 *
 * Debug logging helpers for MacsBug.  Formats "label: value" into
 * a Pascal string and calls DebugStr with a leading semicolon so
 * MacsBug logs the message and continues without stopping.
 */

#include "macsurf_debug.h"

#ifdef MACSURF_DEBUG

#include <string.h>

#ifdef __MACOS9__

void
macsurf_debug_log_int(const char *label, long value)
{
	unsigned char pstr[256];
	char buf[240];
	long v;
	int neg;
	char digits[12];
	int di;
	int len;
	int i;

	if (label == NULL) label = "?";
	len = 0;

	/* Copy label */
	while (label[len] != '\0' && len < 200) {
		buf[len] = label[len];
		len++;
	}
	buf[len++] = ':';
	buf[len++] = ' ';

	/* Convert value to decimal */
	v = value;
	neg = 0;
	if (v < 0) { neg = 1; v = -v; }
	di = 0;
	do {
		digits[di++] = (char)('0' + (int)(v % 10));
		v /= 10;
	} while (v > 0 && di < 11);
	if (neg) digits[di++] = '-';

	for (i = di - 1; i >= 0 && len < 239; i--) {
		buf[len++] = digits[i];
	}
	buf[len] = '\0';

	/* Build Pascal string with leading semicolon for log-and-continue */
	pstr[0] = (unsigned char)(len + 1);
	pstr[1] = ';';
	memcpy(pstr + 2, buf, len);

	DebugStr(pstr);
}

void
macsurf_debug_log_str(const char *label, const char *value)
{
	unsigned char pstr[256];
	char buf[240];
	int len;

	if (label == NULL) label = "?";
	if (value == NULL) value = "(null)";
	len = 0;

	while (label[len] != '\0' && len < 120) {
		buf[len] = label[len];
		len++;
	}
	buf[len++] = ':';
	buf[len++] = ' ';

	{
		int vi = 0;
		while (value[vi] != '\0' && len < 239) {
			buf[len++] = value[vi++];
		}
	}
	buf[len] = '\0';

	pstr[0] = (unsigned char)(len + 1);
	pstr[1] = ';';
	memcpy(pstr + 2, buf, len);

	DebugStr(pstr);
}

#else
/* Linux fallback */
#include <stdio.h>

void
macsurf_debug_log_int(const char *label, long value)
{
	fprintf(stderr, "MS_LOG: %s: %ld\n",
			label != NULL ? label : "?", value);
}

void
macsurf_debug_log_str(const char *label, const char *value)
{
	fprintf(stderr, "MS_LOG: %s: %s\n",
			label != NULL ? label : "?",
			value != NULL ? value : "(null)");
}
#endif /* __MACOS9__ */

#endif /* MACSURF_DEBUG */
