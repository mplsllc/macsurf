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

/* fixes530c: self-enable the file log in this TU only, so the real
 * implementation below (gated on #ifdef MACSURF_DEBUG) compiles in
 * WITHOUT editing macsurf_prefix.h (which forces a full rebuild).
 * Public functions are declared unconditionally in the header, so
 * other TUs link against the real bodies here.  Without this the
 * prefix lacks MACSURF_DEBUG, init is an empty stub, g_log_open stays
 * 0, and no MacSurf Debug.log is created. */
#ifndef MACSURF_RELEASE
#ifndef MACSURF_DEBUG
#define MACSURF_DEBUG 1
#endif
#endif

/* fixes765 (2.0 release) — the temporary #206 SSL/TLS channel and the fixes688
 * perf channel are OFF for release. Their investigations (macintoshgarden http
 * downgrade -> fixes739; the typing-latency perf pass -> fixes760) are done, so
 * the release log is back to failures-only. Re-enable either #define below to
 * bring the networking or timing lines back through the crash-only gate. */
/* #define MACSURF_SSL_LOG 1 */
/* #define MACSURF_PERF_LOG 1 */

/* ============ fixes716 — NUCLEAR DIAGNOSTIC MODE (#207) ============
 * ONE switch to capture EVERYTHING, hang-proof, for root-causing the
 * blank-page bug (and anything else the machine is silently skipping).
 * It:
 *   1. Opens the fixes675 crash-only gate AND every per-category
 *      sub-gate (layout / paint / redraw / fetch), so every log line
 *      the whole app emits is kept — not just crash/NAV/DIAG lines.
 *   2. Turns on the runtime high-frequency trace channel
 *      (macsurf_debug_log_trace_enabled) at init.
 *   3. Flushes EVERY line straight to disk (buffer flush + FlushVol),
 *      not just the first 250 — so a freeze / blank / hang leaves the
 *      COMPLETE trace on disk with nothing stuck in the RAM buffer.
 *
 * This is a DIAGNOSTIC build, not a release: it is slow (per-line HFS
 * sync) and produces very large logs. That is the point — completeness
 * over speed. Self-enabled in this TU, so ONLY macsurf_debug_log.c
 * needs reshipping (no full rebuild). DELETE this one define to return
 * the build to normal crash-only logging. */
/* fixes719c: NUCLEAR OFF. It flushed every log line to disk (FlushVol per
 * line) — ~15k synced writes per browsing session — which crippled the G3/
 * QEMU to a "frozen" crawl. It did its job (captured the blank-screen bug,
 * now fixed by fixes719). Back to crash-only logging: fast, still keeps the
 * RECON HEAP BOUNDS line + crash forensics. Re-enable only to capture a new
 * hard bug. */
/* #define MACSURF_NUCLEAR_LOG 1 */

#ifdef MACSURF_NUCLEAR_LOG
#ifndef MACSURF_VERBOSE_LOG
#define MACSURF_VERBOSE_LOG 1
#endif
#ifndef MACSURF_VERBOSE_LAYOUT_LOG
#define MACSURF_VERBOSE_LAYOUT_LOG 1
#endif
#ifndef MACSURF_VERBOSE_PAINT_LOG
#define MACSURF_VERBOSE_PAINT_LOG 1
#endif
#ifndef MACSURF_VERBOSE_REDRAW_LOG
#define MACSURF_VERBOSE_REDRAW_LOG 1
#endif
#ifndef MACSURF_VERBOSE_FETCH_LOG
#define MACSURF_VERBOSE_FETCH_LOG 1
#endif
#endif /* MACSURF_NUCLEAR_LOG */

#include "macsurf_debug_log.h"

#include <string.h>
#include <stdarg.h>

/* fixes366c — per-navigation reset hooks for the diagnostic
 * counters in redraw.c and plotters.c. Both are defined
 * unconditionally in their own .c files (no MACSURF_DEBUG guard),
 * so they link cleanly in release builds even when MACSURF_DEBUG
 * is off. Forward-declared here rather than via a new header to
 * keep the patch contained to these three files. */
extern void macos9_redraw_diag_counters_reset(void);
extern void macos9_plotter_diag_counters_reset(void);

/* fixes366h — runtime gate for high-frequency trace logging. Defined
 * outside the MACSURF_DEBUG guard so both debug and release builds
 * link, and so a release build's empty tracef stub still satisfies the
 * extern in macsurf_debug_log.h. Default 0 (traces suppressed). */
int macsurf_debug_log_trace_enabled = 0;

/* fixes160a — SITE diagnostic counters. Defined at the top level
 * (outside MACSURF_DEBUG guards) so that html.c's extern references
 * still link in release builds, even though the log itself becomes
 * a no-op. Updated cross-module by image decode (img_ok / img_fail),
 * HTTP fetcher (http_body / http_status), and box-
 * convert in html.c (box_* set). Read once per reformat by the SITE
 * summary line. Globals because pages don't overlap on this
 * single-threaded cooperative app. */
long macsurf__site_box_total = 0;
long macsurf__site_box_blk = 0;
long macsurf__site_box_inlinec = 0;
long macsurf__site_box_inline = 0;
long macsurf__site_box_text = 0;
long macsurf__site_box_other = 0;
/* fixes352 (#107) — last reformat duration in ms (captured in
 * html_reformat after nsu_getmonotonic_ms). Surfaced by about:perf. */
long macsurf__site_reformat_ms = 0;
long macsurf__site_img_ok = 0;
long macsurf__site_img_fail = 0;
/* fixes160d — CSS oversize-gate counters. ok = sheet small enough,
 * parsed and added to cascade; skip = sheet over MACOS9_CSS_MAX_BYTES,
 * dropped before libcss saw it. */
long macsurf__site_css_ok = 0;
long macsurf__site_css_skip = 0;
/* fixes268 (#9) — total CSS bytes seen across all sheets this page.
 * Reset in html_create alongside the other site_ counters. Compared
 * against a 384 KB cap in nscss_process_data so a stack of vendor
 * sheets can't blow the libcss memory budget. */
unsigned long macsurf__site_css_total_bytes = 0;
/* fixes268 (#11) — blocker enum + heavy mode latch
 * (heavy already exists as macsurf__site_heavy below).
 * Blocker values: 0=none, 1=img_budget, 2=css_budget, 3=fetch_slots,
 * 4=fonts, 5=cpu. Latched once when crossed; later overrides only if
 * higher priority. */
long macsurf__site_blocker = 0;
/* fixes161a — resource governor counters. rgov_skip_* are bumped by
 * macos9_http_setup whenever per-class or global active caps refuse a
 * fetch (sub-resource skipped, never reaches libcss / image content
 * handler). fetch_active_peak tracks high-water-mark of active slots
 * across the page load. fetch_slot_fail counts old MAX_F=64 array
 * exhaustion events (should stay 0 with caps in place). heavy is a
 * latched 0/1 marker for fixes161f's heavy-site mode. */
long macsurf__site_rgov_skip_doc = 0;
long macsurf__site_rgov_skip_css = 0;
long macsurf__site_rgov_skip_img = 0;
long macsurf__site_rgov_skip_script = 0;
long macsurf__site_rgov_skip_font = 0;
long macsurf__site_rgov_skip_other = 0;
long macsurf__site_fetch_active_peak = 0;
long macsurf__site_fetch_slot_fail = 0;
long macsurf__site_heavy = 0;
/* fixes161b — global decoded image memory budget.
 * decoded_img_bytes_current is a running sum of width*height*4 across
 * every live decoded bitmap. Incremented at decode time, decremented
 * in macos9_qt_image_destroy. decoded_img_bytes_peak tracks the
 * high-water mark across the page load. decoded_img_skip_budget counts
 * images we refused to decode because doing so would have exceeded
 * MACOS9_DECODED_IMG_MAX_BYTES — these surface as placeholders
 * alongside the per-image oversize gate. These counters are global
 * (process-wide), not per-page, so the decrement matches the increment
 * even across navigations. */
long macsurf__decoded_img_bytes_current = 0;
long macsurf__site_decoded_img_bytes_peak = 0;
long macsurf__site_decoded_img_skip_budget = 0;

#ifdef MACSURF_DEBUG

#ifdef __MACOS9__

#include <MacTypes.h>
#include <MacMemory.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <Processes.h>   /* fixes641 (#197) — app-relative log location */
#include <DateTimeUtils.h>
#include <OSUtils.h>
#include <Events.h>   /* fixes233 — TickCount() for log timestamps */
#include <Timer.h>    /* fixes366a — Microseconds() / UnsignedWide */

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

/* fixes261 — forward declaration so log_close / log_flush can call
 * the buffer-flush helper before its definition (which lives near the
 * log_write impl that owns the buffer). */
static void macsurf_debug_log_buffer_flush(void);

/* fixes895 (reconvert-crash hunt) — narrow dev gate, self-defined HERE so the
 * hunt is armed/disarmed by reshipping ONLY this one file (no prefix edit, no
 * full rebuild). When defined, the crash-only gate additionally keeps the
 * "WORK reconvert" (box-build window) and "WORK timer" (QuickJS realm/timer
 * teardown) breadcrumbs, so both crashes are visible on a shipped-style build
 * WITHOUT opening the whole WORK / verbose firehose. Remove this line to return
 * the log to crash-basic. The breadcrumb CODE ships unconditionally and is
 * runtime-gated by macos9_reconvert_in_progress; this macro only controls
 * whether the emitted lines reach disk.
 *
 * fixes904 — OFF now that the reconvert crash is closed (fixes900-903). This
 * both returns the log to crash-basic (drops the WORK reconvert/timer flood)
 * AND -- via the guards on macsurf_reconv_pos_flush / _reconv_flush below --
 * kills the per-node FlushVol cost, which the SYNCHRONOUS reconvert (fixes903)
 * had packed into one blocking pass (~150 volume syncs per rebuild = a large
 * chunk of the "sluggish"). Re-`#define MACSURF_VERBOSE_RECONVERT 1` to re-arm
 * the whole hunt (log lines + durable ReconvPos marker) if it ever recurs. */
/* #define MACSURF_VERBOSE_RECONVERT 1 */

/* fixes895 — phase-scoped eager flush. While non-zero, macsurf_debug_log_write
 * FlushVols every line it keeps, so the in-flight reconvert/timer breadcrumbs
 * are on disk in order even if the trailing 4 KB buffer is lost to a hard bomb.
 * Armed by html_reconvert (via macsurf_debug_log_reconv_flush(1)) and cleared
 * in html_reconvert_done, so the FlushVol cost is bounded to the reconvert
 * window (breadcrumbs are function/batch level, not per node). */
static int g_log_reconv_flush = 0;

void
macsurf_debug_log_reconv_flush(int on)
{
#ifdef MACSURF_VERBOSE_RECONVERT
	g_log_reconv_flush = on ? 1 : 0;
#else
	(void) on;   /* fixes904 — trace off: never arm the per-line FlushVol */
#endif
}

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

/* fixes641 (#197): resolve the running application's own directory so the log
 * lands next to MacSurf.app, not on the boot volume's Desktop. Duplicated
 * static (same helper as macos9_disk_cache.c — the codebase already duplicates
 * small statics like macos9_ua_for_host rather than adding a shared TU). */
#ifdef __MACOS9__
/* fixes647: shared MacSurfData resolver, defined in macos9_disk_cache.c.
 * The log nests in MacSurfData/ like everything else, with a fallback to
 * the app dir / Desktop below so the crash channel stays robust. */
extern OSErr macos9_data_dir_get(const char *subfolder,
		short *vRef, long *dirID);

static OSErr macos9_app_dir_get(short *vRef, long *dirID)
{
	ProcessSerialNumber psn;
	ProcessInfoRec      info;
	FSSpec              appSpec;
	OSErr               err;

	psn.highLongOfPSN = 0;
	psn.lowLongOfPSN  = kCurrentProcess;
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(ProcessInfoRec);
	info.processName = NULL;
	info.processAppSpec = &appSpec;
	err = GetProcessInformation(&psn, &info);
	if (err != noErr) return err;
	*vRef = appSpec.vRefNum;
	*dirID = appSpec.parID;
	return noErr;
}
#endif

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

	/* fixes647 (#197): log lives in the shared MacSurfData folder. Fall back
	 * to the app dir, then the boot Desktop, so the crash channel survives
	 * even if MacSurfData can't be created. */
	err = macos9_data_dir_get(NULL, &vRefNum, &dirID);
	if (err != noErr) {
		err = macos9_app_dir_get(&vRefNum, &dirID);
		if (err != noErr) {
			err = FindFolder(kOnSystemDisk, kDesktopFolderType,
					kDontCreateFolder, &vRefNum, &dirID);
			if (err != noErr) {
				macsurf_debug_set_title("log init: dir fail");
				return;
			}
		}
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

#ifdef MACSURF_NUCLEAR_LOG
	/* NUCLEAR (#207): turn on the runtime high-frequency trace channel so
	 * every macsurf_debug_log_tracef() site emits too. */
	macsurf_debug_log_trace_enabled = 1;
#endif

	(void)SetFPos(g_log_ref, fsFromLEOF, 0);

	/* fixes703 — prominent, unmistakable session header so a log that spans
	 * several launches (the reporter didn't clear it) is trivially segmented.
	 * Every line is '='-led so it survives the crash-only gate (fixes697).
	 * __DATE__/__TIME__ stamp the BUILD (distinguishes two different .sit's);
	 * the GetDateTime line stamps this LAUNCH. */
	macsurf_debug_log_write(
		"====================================================================");
	macsurf_debug_log_write(
		"=====================   N E W   S E S S I O N   ====================");
	macsurf_debug_log_writef(
		"===  MacSurf 2.0.5   (build %s %s)", __DATE__, __TIME__);
	{
		unsigned long secs = 0;
		DateTimeRec dtr;
		GetDateTime(&secs);
		SecondsToDate(secs, &dtr);
		macsurf_debug_log_writef(
			"===  launched %d-%d-%d  %d:%d:%d",
			(int)dtr.year, (int)dtr.month, (int)dtr.day,
			(int)dtr.hour, (int)dtr.minute, (int)dtr.second);
	}
	macsurf_debug_log_write(
		"====================================================================");
	macsurf_debug_log_writef("log init OK vref=%d dirID=%ld fsref=%d",
		(int)vRefNum, (long)dirID, (int)g_log_ref);
	/* fixes96 — explicit checkpoint: the startup banner needs to be
	 * on disk if a later crash happens during the first event loop. */
	if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
	macsurf_debug_set_title("log OK");
#endif
}

void
macsurf_debug_log_close(void)
{
#ifdef __MACOS9__
	if (!g_log_open) return;
	macsurf_debug_log_write("---- macsurf_debug_log_close ----");
	macsurf_debug_log_buffer_flush();   /* fixes261 */
	(void)FSClose(g_log_ref);
	g_log_ref = 0;
	g_log_open = 0;
#endif
}

/* fixes720 — read the current log back into the caller's buffer. Flushes the
 * buffer first, reads from the module's OWN open fork (avoids a second open on
 * a busy file), then restores the append position. Returns bytes read (out is
 * NUL-terminated); 0 if the log isn't open. Used by File > Send Debug Log to
 * POST the log straight to the server from MacSurf itself. */
long
macsurf_debug_log_read(char *out, long cap)
{
#ifdef __MACOS9__
	long eof = 0;
	long count;
	if (out == NULL || cap <= 0) return 0;
	out[0] = '\0';
	if (!g_log_open || g_log_ref == 0) return 0;
	macsurf_debug_log_buffer_flush();
	if (GetEOF(g_log_ref, &eof) != noErr) return 0;
	if (SetFPos(g_log_ref, fsFromStart, 0) != noErr) return 0;
	count = eof;
	if (count > cap - 1) count = cap - 1;
	if (FSRead(g_log_ref, &count, out) != noErr && count == 0) {
		(void)SetFPos(g_log_ref, fsFromLEOF, 0);
		return 0;
	}
	if (count < 0) count = 0;
	if (count > cap - 1) count = cap - 1;
	out[count] = '\0';
	(void)SetFPos(g_log_ref, fsFromLEOF, 0);   /* restore append position */
	return count;
#else
	(void)out; (void)cap;
	return 0;
#endif
}

/* fixes261 — buffered log writes. Each MS_LOG previously fired 3
 * separate FSWrite calls (timestamp, message, CR) at ~100-200 us
 * Toolbox overhead each. ~3000 lines per cold load = ~1 s of FSWrite
 * wall-clock. Buffering to 4 KB chunks collapses that to ~10 FSWrites
 * total per load (~2 ms). Flush triggers: buffer full, or explicit
 * macsurf_debug_log_flush (still calls FlushVol after for crash
 * durability checkpoint), or session close.
 *
 * The buffer is line-oriented at append: we accumulate timestamp + msg
 * + CR for each call, never splitting a line across two flushes. If
 * the line would overflow, we flush what's there first, then write
 * the new line. Pathologically-long messages (>buffer cap) write
 * directly via FSWrite, bypassing the buffer. */
#define LOG_BUF_CAP 4096
static char g_log_buf[LOG_BUF_CAP];
static long g_log_buf_pos = 0;

/* fixes704 — durable EARLY logging. The first LOG_EAGER_LINES actually-
 * written lines of a session flush straight to disk (buffer flush +
 * FlushVol) so a freeze/hang during startup still leaves the very
 * beginning on disk. After that we fall back to the fast buffered path
 * (fixes261) so page loads aren't slowed by per-line HFS sync. OS 9 is one
 * process per launch, so the file-scope initializer sets this once per run. */
#define LOG_EAGER_LINES 250
static long g_log_eager_left = LOG_EAGER_LINES;

static void
macsurf_debug_log_buffer_flush(void)
{
#ifdef __MACOS9__
	long count;
	if (!g_log_open || g_log_buf_pos <= 0) return;
	count = g_log_buf_pos;
	(void)FSWrite(g_log_ref, &count, g_log_buf);
	g_log_buf_pos = 0;
#endif
}

/* fixes675 — RELEASE crash-only logging. For the shipping 1.68 build the
 * diagnostic channel keeps ONLY crash / fault / guard reports (the durable
 * post-crash forensic trace) and drops every perf / render / fetch / JS
 * diagnostic. This returns non-zero for a line worth keeping. NAV is kept as
 * crash context (what page was loading when it died). Dev builds define
 * MACSURF_VERBOSE_LOG to bypass this gate and get the full filtered trace. */
static int
macsurf_log_is_crash_report(const char *m)
{
	/* fixes697 — the startup banner ("=== MacSurf session Y-M-D H:M:S ==="
	 * and the two "====" rules around it) is the marker that tells a
	 * reporter one log spans multiple launches. It was being dropped by
	 * this crash-only gate (matches no crash keyword). Keep any '='-led
	 * line: 3 lines per session, zero per-page volume. */
	if (m[0] == '=') return 1;
	if (m[0] == 'N' && strncmp(m, "NAV", 3) == 0) return 1;
	if (strstr(m, "FAIL") != NULL) return 1;
	if (strstr(m, "ERROR") != NULL) return 1;
	if (strstr(m, "ASSERT") != NULL) return 1;
	if (strstr(m, "PANIC") != NULL) return 1;
	if (strstr(m, "UAF") != NULL) return 1;
	if (strstr(m, "INVALID") != NULL) return 1;
	if (strstr(m, "ABORT") != NULL) return 1;
	if (strstr(m, "NOMEM") != NULL) return 1;
	if (strstr(m, "CORRUPT") != NULL) return 1;
	/* fixes907 -- talloc heap corruption + the reconvert double-free hunt's
	 * tree-free breadcrumbs. A legitimate crash-forensics class, so it stays
	 * in the permanent crash-only gate (not behind a verbose macro). */
	if (strstr(m, "TALLOC") != NULL) return 1;
	/* fixes911 -- coarse LIFECYCLE trail. The crash-only gate kept NAV + FAIL
	 * and essentially nothing else, so a post-mortem log showed WHERE we
	 * navigated but never what the engine was doing when it died: the
	 * death-row drain crash arrived with an ordinary NAV as its last line.
	 * "LIFE " lines are deliberately COARSE (per drain / per teardown phase,
	 * never per object), so they stay affordable now that debug builds
	 * FlushVol every kept line. */
	if (strstr(m, "LIFE ") != NULL) return 1;
	/* fixes765 (2.0 release): keep ONLY the one-line-per-launch #207
	 * partition-bounds diagnostic (still useful if a field user with a
	 * high-RAM Mac reports a blank page). All the other RECON reconnaissance
	 * (per-nav MEM/RX, the retired KEY/TAref/PAINT/OVL perf probes) and the
	 * per-launch DIAG memory readings are dropped for release — re-enable via
	 * MACSURF_NUCLEAR_LOG / MACSURF_VERBOSE_LOG when diagnosing a new bug. */
	if (strstr(m, "RECON HEAP BOUNDS") != NULL) return 1;
	/* fixes769 (#232) — let the mime-recon lines through the crash-only
	 * gate so a downloaded-instead-of-rendered page is diagnosable. */
	if (strstr(m, "RECON MIME") != NULL) return 1;
	/* fixes953 — the ONE bring-up line kept for release. MacSurf now runs on
	 * two operating systems, so every bug report has to say which. This is a
	 * single line per launch reporting the host OS version; the rest of the
	 * OS X bring-up channels (BOOT / RECON OT / RECON GDEV / PROBE) are
	 * retired now that the tier has closed. Their emitters remain in the
	 * source but no longer pass this gate, so re-enabling any of them is a
	 * one-line change if a new OS X problem needs chasing. */
	if (strstr(m, "RECON OS") != NULL) return 1;
	/* WORK: dedicated development channel for whatever feature/bug is
	 * currently under work. fixes893 (2.0.5 release): gated behind
	 * MACSURF_WORK_LOG (see the note at the passthrough in
	 * macsurf_debug_log_write) so a shipped build stays crash-basic. Undefined
	 * here = a "WORK " line is kept ONLY if it also matches a real crash/
	 * failure keyword above, exactly like any other line. */
#ifdef MACSURF_WORK_LOG
	if (strstr(m, "WORK ") != NULL) return 1;
#endif
	/* fixes895 — the reconvert-crash hunt channel. Keep ONLY the two
	 * breadcrumb families (box-build window + QuickJS timer/realm teardown)
	 * so both crashes are diagnosable on a shipped-style build, without the
	 * MACSURF_WORK_LOG firehose. */
#ifdef MACSURF_VERBOSE_RECONVERT
	if (strstr(m, "WORK reconvert") != NULL) return 1;
	if (strstr(m, "WORK timer") != NULL) return 1;
#endif
	if (strstr(m, "WATCHDOG") != NULL) return 1;
	if (strstr(m, "TERMINAL") != NULL) return 1;
	if (strstr(m, "exception") != NULL) return 1;
	if (strstr(m, "crash") != NULL) return 1;
#ifdef MACSURF_SSL_LOG
	/* fixes677 (#206) — SSL/fetch investigation. Surface the networking
	 * lines so a UA-based or TLS-based http downgrade is visible: the
	 * request line (`https: REQ ... ua=%s`), the handshake result, the
	 * response status, and any redirect (`https: NNN redirect -> ...`).
	 * "http" (4-char prefix) catches http:/https:/http_/https_; "hdr"
	 * catches the response-header dump. Remove MACSURF_SSL_LOG (below)
	 * to return the release to crash-only. */
	if (m[0] == 'h' && strncmp(m, "http", 4) == 0) return 1;
	if (m[0] == 'h' && strncmp(m, "hdr", 3) == 0) return 1;
#endif
#ifdef MACSURF_PERF_LOG
	/* fixes688 — perf-work channel: keep the load-complete PERFACC summary
	 * (tls/net/parse/cascade/layout/paint/js/reflows totals) and the [+Nus]
	 * milestone stamps. Both are one-per-load / few-per-load, so they carry
	 * the timing signal without the per-element volume. */
	if (m[0] == 'P' && strncmp(m, "PERFACC", 7) == 0) return 1;
	if (m[0] == '[' && m[1] == '+') return 1;
#endif
	return 0;
}

void
macsurf_debug_log_write(const char *msg)
{
#ifdef __MACOS9__
	long count;
	size_t mlen;
	char tbuf[20];
	int tlen;
	long line_total;
	static unsigned long t0_ticks = 0;
	unsigned long now;

	if (!g_log_open || msg == NULL) return;

	/* WORK channel: active-development diagnostics prefixed "WORK " bypass
	 * EVERY release filter (the crash-only gate AND all the perf/volume
	 * suppressors below) and go straight to the writer, so what we're
	 * currently debugging is never silently dropped.
	 *
	 * fixes893 (2.0.5 release): gated behind MACSURF_WORK_LOG, matching the
	 * MACSURF_SSL_LOG / MACSURF_PERF_LOG dev-channel pattern. It is NOT defined
	 * in a shipped build, so WORK lines fall through to the crash-only gate
	 * below (and survive only if they ALSO carry a genuine failure keyword).
	 * That returns the release log to crash-basic -- the fixes765 (2.0) state
	 * -- instead of the development flood of WORK pipeline/pump/reconvert/hover
	 * lines. Define MACSURF_WORK_LOG in macsurf_prefix.h to re-arm the channel
	 * for the next debugging cycle. */
#ifdef MACSURF_WORK_LOG
	if (strstr(msg, "WORK ") != NULL) goto do_write;
#endif

	/* fixes675 — crash-only gate for the release build. Keeps just the
	 * forensic lines; drops all perf/render/fetch diagnostics. */
#ifndef MACSURF_VERBOSE_LOG
	if (!macsurf_log_is_crash_report(msg)) return;
#endif

	/* fixes250 — silence the layout-engine debug spam by default.
	 * Cold mactrove loads produced ~7300 LAYOUTPHASE/MCOL/FLEXPHASE
	 * lines per page (80% of total log volume), costing ~700-900 ms
	 * of FSWrite overhead on real hardware. Re-enable with
	 * MACSURF_VERBOSE_LAYOUT_LOG defined at compile time. */
#ifndef MACSURF_VERBOSE_LAYOUT_LOG
	if ((msg[0] == 'L' && strncmp(msg, "LAYOUTPHASE", 11) == 0) ||
	    (msg[0] == 'M' && strncmp(msg, "MCOL", 4) == 0) ||
	    (msg[0] == 'F' && strncmp(msg, "FLEXPHASE", 9) == 0)) {
		return;
	}
#endif

	/* fixes251 — silence the per-paint-element diagnostic spam. After
	 * fixes250 silenced layout debug, the new top categories were
	 * plot_rect[N] (1919/cold-load), plot_text[N] (549), svg_box (152),
	 * slot[N] (142), GRADIENT (115), svg_shape (92). All per-element
	 * paint or per-CSS-slot diagnostics, useful when debugging the
	 * renderer/cascade, useless during normal operation. ~3000 more
	 * lines saved per cold load. Re-enable with MACSURF_VERBOSE_PAINT_LOG. */
#ifndef MACSURF_VERBOSE_PAINT_LOG
	if ((msg[0] == 'p' &&
	     (strncmp(msg, "plot_rect[", 10) == 0 ||
	      strncmp(msg, "plot_text[", 10) == 0)) ||
	    (msg[0] == 's' &&
	     (strncmp(msg, "svg_box:", 8) == 0 ||
	      strncmp(msg, "svg_shape:", 10) == 0 ||
	      strncmp(msg, "svg_walk_enter:", 15) == 0 ||
	      strncmp(msg, "svg_paint:", 10) == 0 ||
	      strncmp(msg, "slot[", 5) == 0)) ||
	    (msg[0] == 'G' && strncmp(msg, "GRADIENT", 8) == 0)) {
		return;
	}
#endif

	/* fixes253 — silence redraw entry context and gui-event spam. Each
	 * redraw produces three "update:" lines (bw=, redraw_ready, and
	 * bw_redraw done); the third has the actual walker counters and
	 * is the only one operationally useful. gw_event: fires per
	 * NetSurf-core GUI event (~200/cold-load). LAYOUT_CRUMB and SITE
	 * url= are per-layout-document markers. All useful for debugging
	 * navigation/redraw issues, not during normal use. ~600 more lines
	 * saved per cold load. Re-enable with MACSURF_VERBOSE_REDRAW_LOG. */
#ifndef MACSURF_VERBOSE_REDRAW_LOG
	if (msg[0] == 'u' && strncmp(msg, "update: ", 8) == 0) {
		/* Keep "update: bw_redraw done ..." (carries walker counters);
		 * drop "update: bw=..." and "update: redraw_ready, ...". */
		if (strncmp(msg + 8, "bw_redraw done", 14) != 0) return;
	}
	if (msg[0] == 'g' && strncmp(msg, "gw_event: e=", 12) == 0) return;
	if (msg[0] == 'L' && strncmp(msg, "LAYOUT_CRUMB", 12) == 0) return;
	/* fixes268 — SITE filter removed. Issues #9 and #11 require the
	 * SITE line to surface css_total= and blocker= for verification.
	 * SITE fires a few times per page reformat (low volume; survivable). */
#endif

	/* fixes666 — perf-cleanup pass. Earlier fix rounds left a pile of
	 * per-element / per-fetch diagnostics that now dominate log volume on a
	 * real page (measured: fixes614 clamp 1048x, plot_bitmap 421x, TBLCOL
	 * 346x, img plot 306x, active fetches 289x, https_teardown 364x, svg_use
	 * 171x per session). They are memcpy'd into the log buffer and flushed in
	 * bulk, but at ~3000+ lines/page they still add up and bury the signal.
	 * Suppress them by prefix, KEEPING the lines that matter for the current
	 * reflow/re-conversion bottleneck: PERFACC, [+Nus] stamps, NAV/SITE,
	 * html_reformat, and box: convert_xml. Re-enable any group with its
	 * MACSURF_VERBOSE_* macro. */
#ifndef MACSURF_VERBOSE_PAINT_LOG
	if (msg[0] == 'p' && strncmp(msg, "plot_bitmap", 11) == 0) return;
	if (msg[0] == 'i' && strncmp(msg, "img plot:", 9) == 0) return;
	if (msg[0] == 's' && strncmp(msg, "svg_use:", 8) == 0) return;
#endif
#ifndef MACSURF_VERBOSE_LAYOUT_LOG
	if (msg[0] == 'T' && strncmp(msg, "TBLCOL", 6) == 0) return;
	if (msg[0] == 'f' && strncmp(msg, "fixes614 clamp:", 15) == 0) return;
#endif
#ifndef MACSURF_VERBOSE_FETCH_LOG
	if (msg[0] == 'h' && (strncmp(msg, "https_teardown:", 15) == 0 ||
			      strncmp(msg, "https: request sent", 19) == 0 ||
			      strncmp(msg, "https: done", 11) == 0 ||
			      strncmp(msg, "https: cookies sent", 19) == 0)) return;
	if (msg[0] == 'a' && strncmp(msg, "active fetches:", 15) == 0) return;
	if (msg[0] == 'e' && strncmp(msg, "evloop: hb", 10) == 0) return;
	/* fixes667 — hlcache/llcache content-lifecycle trace (~965 lines/session).
	 * Left over from the box-walk UAF investigation; not needed for the reflow
	 * bottleneck. Re-enable with MACSURF_VERBOSE_FETCH_LOG if a UAF recurs. */
	if (msg[0] == 'h' && strncmp(msg, "hlcache", 7) == 0) return;
	if (msg[0] == 'l' && strncmp(msg, "llcache", 7) == 0) return;
#endif

	/* fixes669 — perf-focus. Suppress the remaining non-perf diagnostic
	 * categories so the log shows ONLY what bears on load speed. KEEPS:
	 * [+Nus] phase stamps, PERFACC, NAV/SITE, html_reformat, reformat:,
	 * box: convert_xml, https: REQ (network fetches), and error/FAIL lines.
	 * Everything below is fetch-setup / cache-hit / JS-heartbeat / per-image /
	 * per-CSS-slot / DOM-tag detail, useful only when debugging those
	 * subsystems. Re-enable the lot by defining MACSURF_VERBOSE_PERF_OFF. */
#ifndef MACSURF_VERBOSE_PERF_OFF
	if (msg[0] == 'h' && (strncmp(msg, "https_setup", 11) == 0 ||
			      strncmp(msg, "https: CACHE", 12) == 0 ||
			      strncmp(msg, "https: handshake", 16) == 0 ||
			      strncmp(msg, "https: status", 13) == 0 ||
			      strncmp(msg, "https: started", 14) == 0 ||
			      strncmp(msg, "hdr_first", 9) == 0)) return;
	if (msg[0] == 'C' && strncmp(msg, "CACHE", 5) == 0) return;
	if (msg[0] == 'f' && strncmp(msg, "fetch FINISHED", 14) == 0) return;
	if (msg[0] == 'i' && strncmp(msg, "img ", 4) == 0) return;
	if (msg[0] == 'q' && (strncmp(msg, "qjs intr", 8) == 0 ||
			      strncmp(msg, "qjs selftest", 12) == 0)) return;
	if (msg[0] == 'g' && strncmp(msg, "grad cascade", 12) == 0) return;
	if (msg[0] == 'j' && strncmp(msg, "js src", 6) == 0) return;
	if (msg[0] == 't' && strncmp(msg, "tagtype:", 8) == 0) return;
	if (msg[0] == 'n' && strncmp(msg, "nscss", 5) == 0) return;
	if (msg[0] == 's' && strncmp(msg, "sync_cb", 7) == 0) return;
	if (msg[0] == 'c' && (strncmp(msg, "css: budget", 11) == 0 ||
			      strncmp(msg, "css: convert", 12) == 0)) return;
	if (msg[0] == 'b' && strncmp(msg, "broadcast_ready", 15) == 0) return;
	/* fixes672 — more per-element diagnostics found still firing on big pages:
	 * TBLPICK/TBLDIAG (per-table layout picks), fixes569 LEN / fixes446g DATA
	 * (per-cdata-node bad-len probes), gw_invalidate (per-invalidate rects). */
	if (msg[0] == 'T' && (strncmp(msg, "TBLPICK", 7) == 0 ||
			      strncmp(msg, "TBLDIAG", 7) == 0)) return;
	if (msg[0] == 'f' && (strncmp(msg, "fixes569 LEN", 12) == 0 ||
			      strncmp(msg, "fixes446g DATA", 14) == 0)) return;
	if (msg[0] == 'g' && strncmp(msg, "gw_invalidate", 13) == 0) return;
#endif

	/* fixes233 — prepend ms-since-first-log to every line so we can
	 * see where the wall-clock seconds actually go. TickCount is 60 Hz
	 * on OS 9; multiply by 1000/60 = 17 (close enough) for a rough ms
	 * approximation, or print raw ticks and let the reader divide. We
	 * print raw ticks: smaller field, no math, exact. */
do_write:
	now = (unsigned long)TickCount();
	if (t0_ticks == 0) t0_ticks = now;
	tlen = sprintf(tbuf, "[%lu] ", now - t0_ticks);
	if (tlen < 0) tlen = 0;

	mlen = strlen(msg);
	line_total = (long)tlen + (long)mlen + 1;   /* +1 for trailing CR */

	/* fixes261 — buffered append. If the line fits, copy into buffer
	 * (no FSWrite). If not, flush what's there and either re-buffer
	 * or, for pathologically long lines, FSWrite straight through. */
	if (line_total > LOG_BUF_CAP) {
		macsurf_debug_log_buffer_flush();
		if (tlen > 0) {
			count = (long)tlen;
			(void)FSWrite(g_log_ref, &count, tbuf);
		}
		if (mlen > 0) {
			count = (long)mlen;
			(void)FSWrite(g_log_ref, &count, msg);
		}
		count = 1;
		(void)FSWrite(g_log_ref, &count, "\r");
		return;
	}

	if (g_log_buf_pos + line_total > LOG_BUF_CAP) {
		macsurf_debug_log_buffer_flush();
	}
	if (tlen > 0) {
		memcpy(g_log_buf + g_log_buf_pos, tbuf, (size_t)tlen);
		g_log_buf_pos += tlen;
	}
	if (mlen > 0) {
		memcpy(g_log_buf + g_log_buf_pos, msg, mlen);
		g_log_buf_pos += (long)mlen;
	}
	g_log_buf[g_log_buf_pos++] = '\r';

#ifdef MACSURF_NUCLEAR_LOG
	/* NUCLEAR (#207): flush EVERY line straight to disk (buffer flush +
	 * FlushVol). A freeze / blank / hang then loses nothing — the last
	 * line on disk is the last thing that executed. Slow by design. */
	macsurf_debug_log_buffer_flush();
	if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
#else
#ifdef MACSURF_DEBUG
	/* fixes911 — DEBUG builds flush EVERY kept line straight to disk.
	 *
	 * The buffered path (fixes261) and the 250-line eager budget (fixes704)
	 * were both tuned when this channel emitted ~3000 lines per load, where
	 * per-line HFS sync cost seconds. The crash-only gate (fixes675/765) now
	 * keeps ~10-300 lines for a whole SESSION, so the throughput argument is
	 * gone -- but the cost stayed: past line 250 the tail sat in a 4 KB buffer
	 * and a hard crash took it. That is precisely backwards, because the tail
	 * is the only part that matters after a bomb. A real example: an unmapped-
	 * memory exception in the death-row drain (MSL pool header smashed) left a
	 * log whose last line was an ordinary NAV, with nothing about what the
	 * drain was doing.
	 *
	 * At the gate's volume, per-line FlushVol is affordable, and a debug build
	 * should buy durability with it. RELEASE builds keep the old budget +
	 * buffered path below. */
	macsurf_debug_log_buffer_flush();
	if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
	if (g_log_eager_left > 0) g_log_eager_left--;
#else
	/* fixes704 — flush the first LOG_EAGER_LINES lines straight to disk so a
	 * freeze/hang during startup still leaves the very beginning on disk.
	 * After the budget is spent we return to the fast buffered path. */
	if (g_log_eager_left > 0) {
		g_log_eager_left--;
		macsurf_debug_log_buffer_flush();
		if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
	} else if (g_log_reconv_flush) {
		/* fixes895 — while a reconvert/timer flush is in flight, force the
		 * breadcrumb to disk in order so a hard bomb loses nothing after it. */
		macsurf_debug_log_buffer_flush();
		if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
	}
#endif /* MACSURF_DEBUG (fixes911) */
#endif /* MACSURF_NUCLEAR_LOG */

	/*
	 * fixes96 — per-write FlushVol REMOVED. It was synchronously
	 * waiting for HFS to commit each line, which on real hardware
	 * costs ~10–50 ms per call. With ~80 MS_LOG calls per page
	 * load (most from NetSurf core's llcache / hlcache trace), that
	 * was 1–4 seconds of disk-sync wait per nav — the chief cause
	 * of "second nav held back" and the residual dial-up feel.
	 *
	 * The log is now written through HFS's normal cache; clean
	 * shutdown flushes via FSClose, and macsurf_debug_log_flush()
	 * exposes an explicit checkpoint for callers who really need
	 * durability after a critical message. On a hard crash, we may
	 * lose up to a few KB of trailing lines — acceptable given the
	 * stability we've reached and the massive throughput win.
	 */
#else
	if (msg == NULL) return;
	fprintf(stderr, "MS_FLOG: %s\n", msg);
#endif
}

/*
 * fixes96 — explicit flush. Use sparingly. Called once after init
 * so the startup banner is on disk before any work begins.
 */
void
macsurf_debug_log_flush(void)
{
#ifdef __MACOS9__
	if (!g_log_open) return;
	macsurf_debug_log_buffer_flush();   /* fixes261 */
	if (g_log_vref != 0) (void)FlushVol(NULL, g_log_vref);
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

/* ------------------------------------------------------------------
 * fixes895 — reconvert-crash hunt: durable single-slot "furthest
 * position reached" marker.
 *
 * The box-build window (dom_to_box -> convert_xml_to_box_inner ->
 * html_reconvert_done -> content__reformat) is where the reconvert
 * crash kills the session, and the 4 KB log buffer loses its tail on
 * a hard bomb -- so even "rc=0 was the last line" is unreliable.
 * macsurf_reconv_pos_set() records the current phase/node into RAM
 * (per-node, no I/O); macsurf_reconv_pos_flush() rewrites that ONE
 * line into "MacSurf ReconvPos.txt" and FlushVols it, called only at
 * batch/phase boundaries. After a bomb this file names the exact
 * function-phase, reconvert seq, furthest node index, and node tag the
 * Mac was in -- at 100-node granularity -- independent of the main log.
 * ------------------------------------------------------------------ */

static char g_reconv_pos[256] = "reconv-pos (never set)";

void
macsurf_reconv_pos_set(const char *phase, long seq, long node_ix,
		const char *tag)
{
	int pos = 0;
	int cap = (int)sizeof(g_reconv_pos);

	fmt_append_str(g_reconv_pos, cap, &pos, "reconv-pos phase=");
	fmt_append_str(g_reconv_pos, cap, &pos, (phase != NULL) ? phase : "(null)");
	fmt_append_str(g_reconv_pos, cap, &pos, " seq=");
	fmt_append_long(g_reconv_pos, cap, &pos, seq);
	fmt_append_str(g_reconv_pos, cap, &pos, " node=");
	fmt_append_long(g_reconv_pos, cap, &pos, node_ix);
	fmt_append_str(g_reconv_pos, cap, &pos, " tag=");
	fmt_append_str(g_reconv_pos, cap, &pos, (tag != NULL) ? tag : "");
	g_reconv_pos[pos] = '\0';
}

#ifdef __MACOS9__
#ifdef MACSURF_VERBOSE_RECONVERT   /* fixes904 — only compiled when the hunt is armed */
static short g_pos_ref = 0;
static short g_pos_vref = 0;
static int   g_pos_open = 0;
static int   g_pos_tried = 0;

/* Lazily create/open MacSurf ReconvPos.txt in the same MacSurfData dir the
 * log uses, with the same app-dir/Desktop fallback chain. Tried at most once;
 * a failure leaves the marker silently inert (never crashes the hunt). */
static void
macsurf_reconv_pos_open(void)
{
	FSSpec spec;
	OSErr err;
	short vRefNum;
	long dirID;
	unsigned char fname[32];
	const char *name;
	size_t nlen;

	if (g_pos_open || g_pos_tried) return;
	g_pos_tried = 1;

	vRefNum = 0;
	dirID = 0;
	err = macos9_data_dir_get(NULL, &vRefNum, &dirID);
	if (err != noErr) {
		err = macos9_app_dir_get(&vRefNum, &dirID);
		if (err != noErr) {
			err = FindFolder(kOnSystemDisk, kDesktopFolderType,
					kDontCreateFolder, &vRefNum, &dirID);
			if (err != noErr) return;
		}
	}

	name = "MacSurf ReconvPos.txt";
	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRefNum, dirID, fname, &spec);
	if (err == fnfErr) {
		if (FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript) != noErr)
			return;
		err = FSMakeFSSpec(vRefNum, dirID, fname, &spec);
	}
	if (err != noErr) return;
	if (FSpOpenDF(&spec, fsRdWrPerm, &g_pos_ref) != noErr) {
		g_pos_ref = 0;
		return;
	}
	g_pos_vref = vRefNum;
	g_pos_open = 1;
}
#endif /* MACSURF_VERBOSE_RECONVERT (fixes904) */

void
macsurf_reconv_pos_flush(void)
{
	/* fixes904 — trace off (MACSURF_VERBOSE_RECONVERT undefined): the whole
	 * durable-marker body compiles out, so there is NO per-node FlushVol. The
	 * synchronous reconvert (fixes903) ran ~150 of these in one blocking pass;
	 * removing them is a real speedup, not just log hygiene. Re-arm the macro
	 * to restore the durable ReconvPos.txt marker for a future crash hunt. */
#ifdef MACSURF_VERBOSE_RECONVERT
	long count;
	size_t plen;

	if (!g_pos_open)
		macsurf_reconv_pos_open();
	if (!g_pos_open)
		return;

	plen = strlen(g_reconv_pos);
	(void)SetFPos(g_pos_ref, fsFromStart, 0);
	count = (long)plen;
	(void)FSWrite(g_pos_ref, &count, g_reconv_pos);
	count = 1;
	(void)FSWrite(g_pos_ref, &count, "\r");
	/* truncate any leftover tail from a previous, longer line. */
	(void)SetEOF(g_pos_ref, (long)plen + 1);
	if (g_pos_vref != 0)
		(void)FlushVol(NULL, g_pos_vref);
#endif
}
#else
void
macsurf_reconv_pos_flush(void)
{
	fprintf(stderr, "RECONV-POS: %s\n", g_reconv_pos);
}
#endif

/* fixes895 — largest contiguous free block (bytes), for the H1 double-buffer
 * memory-exhaustion hypothesis: two full box trees + two style sets coexist as
 * the new tree nears completion, so if box_create/talloc_zero is returning NULL
 * on heavy pages this reading should approach 0 near the crash node index.
 * Returns -1 off-Mac (no Memory Manager). PurgeSpace reports max-contiguous,
 * which is what a single large libcss/box alloc actually needs. */
#ifdef __MACOS9__
long
macsurf_free_mem(void)
{
	long total = 0;
	long contig = 0;
	PurgeSpace(&total, &contig);
	return contig;
}
#else
long
macsurf_free_mem(void)
{
	return -1;
}
#endif

/* fixes366h — runtime-gated high-frequency trace. Default off so the
 * per-element var()/grid diagnostics don't pollute profiling captures
 * or the crash-forensic log. Flip the flag at a debugger breakpoint
 * (or from code) to re-enable the firehose for a specific bug hunt. */
void
macsurf_debug_log_tracef(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!macsurf_debug_log_trace_enabled) return;
	va_start(ap, fmt);
	fmt_vformat(buf, (int)sizeof(buf), fmt, ap);
	va_end(ap);

	macsurf_debug_log_write(buf);
}

/* ------------------------------------------------------------------
 * fixes366a — Microsecond profile timer
 *
 * Captures a Microseconds() timestamp into g_profile_t0 (UnsignedWide:
 * UInt32 hi + UInt32 lo). Each stamp call writes one log line
 * "[+NNNNNus] LABEL" with the delta from t0 in integer microseconds.
 *
 * Delta is computed in double — CW8 PPC miscompiles long-long
 * subtraction (project_cw8_longlong_codegen). PPC has a hardware FPU
 * and IEEE 754's 52-bit mantissa exactly represents every UInt32 we
 * can multiply by 2^32, so no precision is lost.
 *
 * Auto-reset on first stamp keeps a missed reset call from emitting
 * giant garbage deltas.
 * ------------------------------------------------------------------ */

#ifdef __MACOS9__
static UnsignedWide g_profile_t0 = { 0, 0 };
#endif
static int g_profile_t0_set = 0;

/* fixes369 (#167) — per-load page-weight + resource-count accounting, the
 * size dimension the timing stamps lacked. Reset at nav start with the
 * timing t0; accumulated by the fetchers; emitted at load-complete as a
 * single parseable PROFILE line that perf/scrape.py folds into
 * perf/history.csv so page weight is measurable run-over-run. */
static long g_profile_bytes = 0;	/* total body bytes this load   */
static int  g_profile_resources = 0;	/* sub-resource fetches this load */

/* fixes640 — TRUSTWORTHY per-load phase accumulators (microseconds).
 *
 * This replaces the fixes637/638 milestone-subtraction scheme, which produced
 * negative/garbage per-phase numbers because it subtracted NON-sequential
 * milestone timestamps (cascade overlaps parse; fetch-finished fires once per
 * sub-resource; first-paint-done repeats). Only total_ms was ever meaningful.
 *
 * Instead, each phase brackets its choke function with macos9_micros() and adds
 * the elapsed delta into one of these accumulators via the macsurf_profile_
 * accum_* adders below. The SUM across the whole load is honest CPU-per-phase,
 * regardless of how the phases interleave. Reset at nav start; emitted once at
 * the real load-complete edge as the PERFACC line. All longs (the writef
 * formatter only supports %d/%ld/%p/%s/%%; a 34s load = 34e6 us << 2.1e9). */
static long g_accum_tls_us     = 0;	/* TLS handshake CPU               */
static long g_accum_net_us     = 0;	/* OTRcv + deliver CPU (not idle)  */
static long g_accum_parse_us   = 0;	/* html tokenize only (not box ctor)*/
static long g_accum_cascade_us = 0;	/* css_select_style, summed        */
static long g_accum_layout_us  = 0;	/* layout_document, summed          */
static long g_accum_paint_us   = 0;	/* browser_window_redraw, summed    */
static long g_accum_js_us      = 0;	/* js_exec, summed                  */
static long g_accum_reflows    = 0;	/* full html_reformat passes        */
static long g_perf_ttfb_us     = -1;	/* first fetch-finished = TTFB     */

static void macsurf_profile_note_milestone(const char *label, int delta_us_i);

#ifdef __MACOS9__
static double
profile_us_from_wide(const UnsignedWide *w)
{
	/* Two 32-bit halves -> double. 4294967296.0 == 2^32. */
	return (double)w->hi * 4294967296.0 + (double)w->lo;
}
#endif

void
macsurf_profile_reset(void)
{
#ifdef __MACOS9__
	Microseconds(&g_profile_t0);
	g_profile_t0_set = 1;
#else
	g_profile_t0_set = 1;
#endif
	/* fixes369 — zero the page-weight + resource counters per load. */
	g_profile_bytes = 0;
	g_profile_resources = 0;
	/* fixes366c — clear the redraw / plotter diag-line throttles
	 * so each navigation gets a fresh window of 5 events per
	 * pattern instead of one shared budget across the whole
	 * session. */
	macos9_redraw_diag_counters_reset();
	macos9_plotter_diag_counters_reset();
	/* fixes640 — fresh phase accumulators per load. */
	g_accum_tls_us     = 0;
	g_accum_net_us     = 0;
	g_accum_parse_us   = 0;
	g_accum_cascade_us = 0;
	g_accum_layout_us  = 0;
	g_accum_paint_us   = 0;
	g_accum_js_us      = 0;
	g_accum_reflows    = 0;
	g_perf_ttfb_us     = -1;
}

void
macsurf_profile_stamp(const char *label)
{
#ifdef __MACOS9__
	UnsignedWide now;
	double us_now;
	double us_t0;
	double delta_us;
	int delta_us_i;

	if (label == NULL) label = "(null)";
	if (g_profile_t0_set == 0) {
		macsurf_profile_reset();
	}
	Microseconds(&now);
	us_now = profile_us_from_wide(&now);
	us_t0 = profile_us_from_wide(&g_profile_t0);
	delta_us = us_now - us_t0;
	if (delta_us < 0.0) delta_us = 0.0;
	delta_us_i = (int)(delta_us + 0.5);
	macsurf_debug_log_writef("[+%dus] %s", delta_us_i, label);
	macsurf_profile_note_milestone(label, delta_us_i);
#else
	if (label == NULL) label = "(null)";
	if (g_profile_t0_set == 0) {
		macsurf_profile_reset();
	}
	macsurf_debug_log_writef("[+0us] %s", label);
	macsurf_profile_note_milestone(label, 0);
#endif
}

/* fixes640 — the ONLY milestone we still derive is TTFB: the first
 * fetch-finished after nav start. On single-burst / cache-hit loads this is
 * the main document; on a cold streaming load a tiny early sub-resource on a
 * fast host can complete first, so treat it as "time to first completed fetch"
 * (a rough TTFB), not an exact main-doc timing. Everything else is measured by
 * accumulators, not milestone subtraction. */
static void
macsurf_profile_note_milestone(const char *label, int delta_us_i)
{
	if (g_perf_ttfb_us < 0 && strcmp(label, "fetch-finished") == 0)
		g_perf_ttfb_us = (long)delta_us_i;
}

/* fixes640 — phase-time adders. Each brackets a choke function with
 * macos9_micros() and passes the elapsed microsecond delta. Guarded
 * non-negative (a clock hiccup or reset mid-bracket can't corrupt the sum). */
void macsurf_profile_accum_tls(long us)     { if (us > 0) g_accum_tls_us     += us; }
void macsurf_profile_accum_net(long us)     { if (us > 0) g_accum_net_us     += us; }
void macsurf_profile_accum_parse(long us)   { if (us > 0) g_accum_parse_us   += us; }
void macsurf_profile_accum_cascade(long us) { if (us > 0) g_accum_cascade_us += us; }
void macsurf_profile_accum_layout(long us)  { if (us > 0) g_accum_layout_us  += us; }
void macsurf_profile_accum_paint(long us)   { if (us > 0) g_accum_paint_us   += us; }
void macsurf_profile_accum_js(long us)      { if (us > 0) g_accum_js_us      += us; }
void macsurf_profile_note_reflow(void)      { g_accum_reflows++; }

/* fixes640a (review blocker): inline <script> executes SYNCHRONOUSLY inside
 * dom_hubbub_parser_parse_chunk (parser script callback -> js_exec -> JS_Eval),
 * so the parse bracket in html_process_data would double-count that JS time in
 * both parse and js. The parse bracket reads this before/after and subtracts
 * the js delta, so parse = pure tokenize and total isn't inflated. */
long macsurf_profile_get_js_us(void)        { return g_accum_js_us; }

/* fixes640 — emit the honest per-phase breakdown ONCE at the load-complete
 * edge (see main.c poll loop). Every field is a summed microsecond total, so
 * they can be compared run-over-run; perf/scrape.py folds them straight into
 * the existing history.csv columns. `total` is the sum of the CPU phases (tls
 * is a subset of net-adjacent handshake CPU; ttfb is wall-clock, reported
 * alongside but NOT summed into total). */
void
macsurf_profile_emit_phases(const char *url)
{
	long total;
	(void)url;
	total = g_accum_tls_us + g_accum_net_us + g_accum_parse_us +
		g_accum_cascade_us + g_accum_layout_us + g_accum_paint_us +
		g_accum_js_us;
	macsurf_debug_log_writef(
		"PERFACC tls=%ldus net=%ldus ttfb=%ldus parse=%ldus cascade=%ldus "
		"layout=%ldus paint=%ldus js=%ldus reflows=%ld total=%ldus",
		g_accum_tls_us, g_accum_net_us,
		(g_perf_ttfb_us < 0) ? -1L : g_perf_ttfb_us,
		g_accum_parse_us, g_accum_cascade_us, g_accum_layout_us,
		g_accum_paint_us, g_accum_js_us, g_accum_reflows, total);
	/* fixes640b — zero the accumulators AFTER emitting so the next load
	 * starts clean regardless of navigation type (URL-bar, link click,
	 * back/forward). This replaces the mid-load set_url reset that was
	 * corrupting the numbers. A settle-reflow that fires after this emit
	 * bleeds a little into the next load's layout/paint — a slight, rare
	 * mis-attribution, far better than the mid-load wipe. */
	g_accum_tls_us     = 0;
	g_accum_net_us     = 0;
	g_accum_parse_us   = 0;
	g_accum_cascade_us = 0;
	g_accum_layout_us  = 0;
	g_accum_paint_us   = 0;
	g_accum_js_us      = 0;
	g_accum_reflows    = 0;
	g_perf_ttfb_us     = -1;
}

/* fixes369 (#167) — page-weight + resource-count accounting. */
void
macsurf_profile_add_bytes(long n)
{
	if (n > 0) g_profile_bytes += n;
}

void
macsurf_profile_count_resource(void)
{
	g_profile_resources++;
}

void
macsurf_profile_emit(const char *url)
{
	if (url == NULL) url = "(null)";
	macsurf_debug_log_writef(
		"PROFILE url=%s total_bytes=%ld subresources=%d",
		url, g_profile_bytes, g_profile_resources);
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
void macsurf_debug_log_tracef(const char *fmt, ...) { (void)fmt; }
void macsurf_profile_reset(void)
{
	/* fixes366c — even in release builds, call the diag-counter
	 * resets so the symbols are referenced and the file-scope
	 * statics in redraw.c / plotters.c don't pin up at 5 across
	 * navigations if a future MACSURF_DEBUG build links the
	 * same objects. The reset bodies are unconditionally
	 * compiled and cheap. */
	macos9_redraw_diag_counters_reset();
	macos9_plotter_diag_counters_reset();
}
void macsurf_profile_stamp(const char *label) { (void)label; }
void macsurf_profile_add_bytes(long n) { (void)n; }
void macsurf_profile_count_resource(void) {}
void macsurf_profile_emit(const char *url) { (void)url; }
/* fixes640 — release no-op stubs for the phase accumulators. */
void macsurf_profile_accum_tls(long us)     { (void)us; }
void macsurf_profile_accum_net(long us)     { (void)us; }
void macsurf_profile_accum_parse(long us)   { (void)us; }
void macsurf_profile_accum_cascade(long us) { (void)us; }
void macsurf_profile_accum_layout(long us)  { (void)us; }
void macsurf_profile_accum_paint(long us)   { (void)us; }
void macsurf_profile_accum_js(long us)      { (void)us; }
void macsurf_profile_note_reflow(void) {}
long macsurf_profile_get_js_us(void) { return 0; }
void macsurf_profile_emit_phases(const char *url) { (void)url; }

#endif /* MACSURF_DEBUG */

/* fixes173 — non-fatal assertion handler. Called by the assert()
 * macro overridden in macsurf_prefix.h. Logs the failure but
 * does NOT abort the process. Real shipped browsers do this
 * (or NDEBUG out entirely). Without it, Apple's first unexpected
 * box-tree state aborts MacSurf before a pixel is drawn.
 *
 * Always exported (even in release builds) so every TU that
 * pulls in macsurf_prefix.h's assert() macro links cleanly. */
void macsurf_assert_failed_(const char *expr, const char *file, int line)
{
#ifdef MACSURF_DEBUG
	macsurf_debug_log_writef("ASSERT %s:%d %s",
			file ? file : "?", line,
			expr ? expr : "?");
#else
	(void)expr; (void)file; (void)line;
#endif
}
