/*
 * MacSurf -- macsurf_debug_log.c  (fixes149)
 *
 * File-backed diagnostic channel. Opens "MacSurf Debug.log" on the
 * Desktop at startup, appends one CR-terminated line per call, and
 * calls FlushVol + an LEOF SetFPos pair after every write so the
 * file survives a hard crash (illegal-instruction, frozen Mac, or
 * forced restart).
 *
 * The log file is the fixes149 deliverable: with no ADB keyboard
 * for MacsBug `wh`, this file is the only way to get a post-crash
 * back trace on the G3 / G4.
 *
 * C89 / CW8 constraints:
 *   - All variables at top of block.
 *   - No // comments.
 *   - No variadic macros, no inline.
 *   - Minimal custom printf (no MSL vsnprintf dependency -- CW8's
 *     Carbon MSL may not link vsnprintf reliably, and the existing
 *     macsurf_debug.c rolls its own integer formatter for the same
 *     reason).
 *
 * Includes only the Mac Toolbox bits we need. The prefix header
 * has already pulled MacTypes.h / stdint.h / the NSLOG block.
 */

#include "macsurf_debug_log.h"

#include <string.h>
#include <stdarg.h>

#ifdef MACSURF_DEBUG

#ifdef __MACOS9__

#include <MacTypes.h>
#include <MacMemory.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <DateTimeUtils.h>
#include <OSUtils.h>

/*
 * CW8's Folders.h generally defines these, but be defensive -- the
 * prefix file pattern for MacSurf is to provide fallbacks for
 * anything CW8 might miss.
 */
#ifndef kOnSystemDisk
#define kOnSystemDisk ((short)-32768)
#endif
#ifndef kDesktopFolderType
#define kDesktopFolderType 'desk'
#endif
#ifndef kDontCreateFolder
#define kDontCreateFolder 0
#endif
#ifndef fnfErr
#define fnfErr -43
#endif
#ifndef fsRdWrPerm
#define fsRdWrPerm 3
#endif
#ifndef fsFromStart
#define fsFromStart 1
#endif
#ifndef fsFromLEOF
#define fsFromLEOF 2
#endif
#ifndef smSystemScript
#define smSystemScript 0
#endif

static short g_log_ref = 0;
static short g_log_vref = 0;
static int   g_log_open = 0;

#else

/*
 * Linux cross-check stubs. No Mac File Manager; everything
 * no-ops. Kept distinct from __MACOS9__ path so the syntax
 * checker does not have to fake FSSpec / FSWrite / etc.
 */
#include <stdio.h>

#endif

/* ------------------------------------------------------------------
 * Minimal formatter
 * ------------------------------------------------------------------ */

static void
fmt_append_char(char *dst, int dst_size, int *pos, char c)
{
	if (*pos < dst_size - 1) {
		dst[*pos] = c;
		(*pos)++;
	}
}

static void
fmt_append_str(char *dst, int dst_size, int *pos, const char *s)
{
	int i;
	if (s == NULL) s = "(null)";
	for (i = 0; s[i] != '\0'; i++) {
		fmt_append_char(dst, dst_size, pos, s[i]);
	}
}

static void
fmt_append_long(char *dst, int dst_size, int *pos, long v)
{
	char digits[12];
	int di;
	int i;
	unsigned long uval;
	int neg;

	neg = 0;
	if (v < 0) {
		neg = 1;
		uval = (unsigned long)(-(v + 1)) + 1UL;
	} else {
		uval = (unsigned long)v;
	}
	di = 0;
	do {
		digits[di++] = (char)('0' + (int)(uval % 10UL));
		uval /= 10UL;
	} while (uval > 0UL && di < 11);
	if (neg) fmt_append_char(dst, dst_size, pos, '-');
	for (i = di - 1; i >= 0; i--) {
		fmt_append_char(dst, dst_size, pos, digits[i]);
	}
}

static void
fmt_append_hex8(char *dst, int dst_size, int *pos, unsigned long v)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;
	for (i = 7; i >= 0; i--) {
		fmt_append_char(dst, dst_size, pos,
			hex[(v >> (i * 4)) & 0xF]);
	}
}

static void
fmt_vformat(char *dst, int dst_size, const char *fmt, va_list ap)
{
	int pos;
	const char *p;

	pos = 0;
	if (dst_size <= 0) return;
	if (fmt == NULL) fmt = "(null-fmt)";
	p = fmt;
	while (*p != '\0') {
		if (*p != '%') {
			fmt_append_char(dst, dst_size, &pos, *p);
			p++;
			continue;
		}
		p++;
		if (*p == '\0') break;
		if (*p == '%') {
			fmt_append_char(dst, dst_size, &pos, '%');
			p++;
		} else if (*p == 'd') {
			long v = (long)va_arg(ap, int);
			fmt_append_long(dst, dst_size, &pos, v);
			p++;
		} else if (*p == 'l' && *(p + 1) == 'd') {
			long v = va_arg(ap, long);
			fmt_append_long(dst, dst_size, &pos, v);
			p += 2;
		} else if (*p == 'p') {
			unsigned long v = (unsigned long)va_arg(ap, void *);
			fmt_append_hex8(dst, dst_size, &pos, v);
			p++;
		} else if (*p == 's') {
			const char *s = va_arg(ap, const char *);
			fmt_append_str(dst, dst_size, &pos, s);
			p++;
		} else {
			fmt_append_char(dst, dst_size, &pos, '%');
			fmt_append_char(dst, dst_size, &pos, *p);
			p++;
		}
	}
	dst[pos] = '\0';
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/* Forward declaration so init can post failure status to the title
 * bar when the file channel itself cannot be opened. Avoids a
 * circular include via macsurf_debug.h. */
extern void macsurf_debug_set_title(const char *msg);

void
macsurf_debug_log_init(void)
{
#ifdef __MACOS9__
	FSSpec spec;
	OSErr err;
	OSErr cerr;
	short vRefNum;
	long dirID;
	unsigned char fname[32];
	const char *name;
	size_t nlen;

	if (g_log_open) return;

	vRefNum = 0;
	dirID = 0;

	err = FindFolder(kOnSystemDisk, kDesktopFolderType,
			kDontCreateFolder, &vRefNum, &dirID);
	if (err != noErr) {
		macsurf_debug_set_title("log init: FindFolder fail");
		return;
	}

	name = "MacSurf Debug.log";
	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRefNum, dirID, fname, &spec);
	if (err == fnfErr) {
		cerr = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
		if (cerr != noErr) {
			macsurf_debug_set_title("log init: FSpCreate fail");
			return;
		}
		err = FSMakeFSSpec(vRefNum, dirID, fname, &spec);
	}
	if (err != noErr) {
		macsurf_debug_set_title("log init: FSMakeFSSpec fail");
		return;
	}

	if (FSpOpenDF(&spec, fsRdWrPerm, &g_log_ref) != noErr) {
		g_log_ref = 0;
		macsurf_debug_set_title("log init: FSpOpenDF fail");
		return;
	}

	g_log_vref = vRefNum;
	g_log_open = 1;

	(void)SetFPos(g_log_ref, fsFromLEOF, 0);

	macsurf_debug_log_write("");
	macsurf_debug_log_write("========================================");
	{
		unsigned long secs = 0;
		DateTimeRec dtr;
		GetDateTime(&secs);
		SecondsToDate(secs, &dtr);
		macsurf_debug_log_writef(
			"=== MacSurf session %d-%d-%d %d:%d:%d ===",
			(int)dtr.year, (int)dtr.month, (int)dtr.day,
			(int)dtr.hour, (int)dtr.minute, (int)dtr.second);
	}
	macsurf_debug_log_write("========================================");
	macsurf_debug_log_writef("log init OK vref=%d dirID=%ld fsref=%d",
		(int)vRefNum, (long)dirID, (int)g_log_ref);
	macsurf_debug_set_title("log OK");
#endif
}

void
macsurf_debug_log_close(void)
{
#ifdef __MACOS9__
	if (!g_log_open) return;
	macsurf_debug_log_write("---- macsurf_debug_log_close ----");
	(void)FSClose(g_log_ref);
	g_log_ref = 0;
	g_log_open = 0;
#endif
}

void
macsurf_debug_log_write(const char *msg)
{
#ifdef __MACOS9__
	long count;
	size_t mlen;

	if (!g_log_open || msg == NULL) return;

	mlen = strlen(msg);
	if (mlen > 0) {
		count = (long)mlen;
		(void)FSWrite(g_log_ref, &count, msg);
	}

	count = 1;
	(void)FSWrite(g_log_ref, &count, "\r");

	/*
	 * Force a flush so the entry survives a crash. The SetFPos
	 * LEOF re-assertion pushes File Manager's cached position
	 * state; FlushVol asks HFS to commit the volume's metadata
	 * and data buffers. Both are needed -- on classic Mac OS
	 * the File Manager caches aggressively, and a crash can
	 * eat up to the last 32 KB of unwritten data without
	 * FlushVol.
	 */
	(void)SetFPos(g_log_ref, fsFromStart, 0);
	(void)SetFPos(g_log_ref, fsFromLEOF, 0);
	if (g_log_vref != 0) {
		(void)FlushVol(NULL, g_log_vref);
	}
#else
	if (msg == NULL) return;
	fprintf(stderr, "MS_FLOG: %s\n", msg);
#endif
}

void
macsurf_debug_log_writef(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	fmt_vformat(buf, (int)sizeof(buf), fmt, ap);
	va_end(ap);

	macsurf_debug_log_write(buf);
}

#else /* !MACSURF_DEBUG */

/*
 * Release-build stubs. MACSURF_DEBUG is undefined when
 * MACSURF_RELEASE is set; all diagnostic surface area compiles
 * out but the symbols stay exported so any release-path call
 * site links.
 */

void macsurf_debug_log_init(void) {}
void macsurf_debug_log_close(void) {}
void macsurf_debug_log_write(const char *msg) { (void)msg; }
void macsurf_debug_log_writef(const char *fmt, ...) { (void)fmt; }

#endif /* MACSURF_DEBUG */
