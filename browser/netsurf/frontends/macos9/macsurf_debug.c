/*
 * MacSurf — macsurf_debug.c
 *
 * MS_LOG writes to the front window title bar. No DebugStr, no
 * debugger stops. Each checkpoint overwrites the previous one so
 * the title always shows the last pipeline stage reached.
 */

#include "macsurf_debug.h"

#ifdef MACSURF_DEBUG

#include <string.h>

/* Global lock flag: when non-zero, macsurf_debug_set_title and the
 * log_int/log_str helpers become no-ops. The _force variants temporarily
 * clear the lock so diagnostic probes can set the title, then re-engage
 * it to prevent downstream callers (redraw counter, etc.) from
 * clobbering the probe output. */
static int g_title_locked = 0;

int
macsurf_debug_title_is_locked(void)
{
	return g_title_locked;
}

/* Probe accumulator: each macsurf_debug_probe_append call extends this
 * buffer with its message and re-asserts the title so all probe output
 * from across the pipeline remains visible as one concatenated string. */
static char g_probe_buf[256];
static int  g_probe_len = 0;

static void
probe_buf_append_char(char c)
{
	if (g_probe_len < (int)sizeof(g_probe_buf) - 1) {
		g_probe_buf[g_probe_len++] = c;
		g_probe_buf[g_probe_len] = '\0';
	}
}

static void
probe_buf_append_str(const char *s)
{
	int i;
	if (s == NULL) return;
	for (i = 0; s[i] != '\0'; i++) {
		probe_buf_append_char(s[i]);
	}
}

static void
probe_buf_append_long(long v)
{
	char digits[12];
	int di;
	int i;
	unsigned long uval;
	int neg;

	neg = 0;
	if (v < 0) {
		neg = 1;
		/* Negate safely even for LONG_MIN (where -v overflows). */
		uval = (unsigned long)(-(v + 1)) + 1UL;
	} else {
		uval = (unsigned long)v;
	}
	di = 0;
	do {
		digits[di++] = (char)('0' + (int)(uval % 10UL));
		uval /= 10UL;
	} while (uval > 0UL && di < 11);
	if (neg) probe_buf_append_char('-');
	for (i = di - 1; i >= 0; i--) {
		probe_buf_append_char(digits[i]);
	}
}

void
macsurf_debug_probe_reset(void)
{
	g_probe_buf[0] = '\0';
	g_probe_len = 0;
}

void
macsurf_debug_probe_append(const char *msg)
{
	if (g_probe_len > 0) probe_buf_append_char(' ');
	probe_buf_append_str(msg);
	macsurf_debug_set_title_force(g_probe_buf);
}

void
macsurf_debug_probe_append_int(const char *label, long value)
{
	if (g_probe_len > 0) probe_buf_append_char(' ');
	probe_buf_append_str(label);
	probe_buf_append_char('=');
	probe_buf_append_long(value);
	macsurf_debug_set_title_force(g_probe_buf);
}

#ifdef __MACOS9__
#include <Files.h>
struct AliasRecord;
typedef struct AliasRecord **AliasHandle;
#include <Aliases.h>
#include <MacWindows.h>

void
macsurf_debug_set_title(const char *msg)
{
	WindowRef w;
	unsigned char pstr[256];
	size_t len;

	if (g_title_locked) return;

	w = FrontWindow();
	if (w == NULL || msg == NULL) return;

	len = strlen(msg);
	if (len > 255) len = 255;
	pstr[0] = (unsigned char)len;
	memcpy(pstr + 1, msg, len);
	SetWTitle(w, pstr);
}

void
macsurf_debug_set_title_force(const char *msg)
{
	g_title_locked = 0;
	macsurf_debug_set_title(msg);
	g_title_locked = 1;
}

void
macsurf_debug_log_int_force(const char *label, long value)
{
	g_title_locked = 0;
	macsurf_debug_log_int(label, value);
	g_title_locked = 1;
}

void
macsurf_debug_log_int(const char *label, long value)
{
	char buf[128];
	unsigned long uv;
	int neg;
	char digits[12];
	int di;
	int len;
	int i;

	if (label == NULL) label = "?";
	len = 0;
	while (label[len] != '\0' && len < 80) {
		buf[len] = label[len];
		len++;
	}
	buf[len++] = ':';
	buf[len++] = ' ';

	neg = 0;
	if (value < 0) {
		neg = 1;
		uv = (unsigned long)(-(value + 1)) + 1UL;
	} else {
		uv = (unsigned long)value;
	}
	di = 0;
	do {
		digits[di++] = (char)('0' + (int)(uv % 10UL));
		uv /= 10UL;
	} while (uv > 0UL && di < 11);
	if (neg) digits[di++] = '-';
	for (i = di - 1; i >= 0 && len < 126; i--)
		buf[len++] = digits[i];
	buf[len] = '\0';

	/* fixes168 -- route to file log instead of title bar. Same
	 * rationale as fixes167's MS_LOG change: diagnostic probes
	 * scattered across NetSurf core (hlcache.c "unaccept #", etc.)
	 * shouldn't clobber the window title. Use
	 * macsurf_debug_log_int_force if title output is intended. */
	macsurf_debug_log_write(buf);
}

void
macsurf_debug_log_str(const char *label, const char *value)
{
	char buf[128];
	int len;
	int vi;

	if (label == NULL) label = "?";
	if (value == NULL) value = "(null)";
	len = 0;
	while (label[len] != '\0' && len < 60) {
		buf[len] = label[len];
		len++;
	}
	buf[len++] = ':';
	buf[len++] = ' ';
	vi = 0;
	while (value[vi] != '\0' && len < 126)
		buf[len++] = value[vi++];
	buf[len] = '\0';

	/* fixes168 -- file log only, same rationale as log_int. */
	macsurf_debug_log_write(buf);
}

#else
#include <stdio.h>

void macsurf_debug_set_title(const char *msg)
{
	if (g_title_locked) return;
	fprintf(stderr, "MS_LOG: %s\n", msg != NULL ? msg : "(null)");
}

void macsurf_debug_log_int(const char *label, long value)
{
	if (g_title_locked) return;
	fprintf(stderr, "MS_LOG: %s: %ld\n", label ? label : "?", value);
}

void macsurf_debug_log_str(const char *label, const char *value)
{
	if (g_title_locked) return;
	fprintf(stderr, "MS_LOG: %s: %s\n", label ? label : "?", value ? value : "(null)");
}

void macsurf_debug_set_title_force(const char *msg)
{
	g_title_locked = 0;
	macsurf_debug_set_title(msg);
	g_title_locked = 1;
}

void macsurf_debug_log_int_force(const char *label, long value)
{
	g_title_locked = 0;
	macsurf_debug_log_int(label, value);
	g_title_locked = 1;
}
#endif
#endif
