/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macsurf_osver.c — runtime host-OS version detection (OS 9 vs Mac OS X).
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * fixes936 (OS X tier 1). See macsurf_osver.h for the full rationale: one
 * CFM Carbon binary runs on both Mac OS 9 and Mac OS X 10.0-10.4, and this
 * is the single place that decides which one we are on.
 *
 * C89 compatible. Uses Carbon Toolbox (Gestalt.h).
 */

#include "macsurf_osver.h"
#include "macsurf_debug_log.h"

#ifdef __MACOS9__
#include <Gestalt.h>
#endif

/* Gestalt selectors spelled out locally so this file compiles against ANY
 * vintage of Universal Interfaces. UI 3.4 -- what CW8 ships, and what
 * Retro68's multiversal set matches -- declares gestaltSystemVersion but NOT
 * the Major/Minor/BugFix selectors, which Mac OS X 10.4 added.
 *
 * PRIVATE names, deliberately: in UI 3.4 these are enum constants, not
 * macros, so an #ifndef guard around the real names would spuriously pass on
 * a newer SDK where they DO exist and then shadow the enum. Same local-
 * definition technique as macos9_image.c's k32ARGBPixelFormat fallback.
 *
 * All four codes are < 2^31, so they convert cleanly to both UI 3.4's
 * unsigned-long OSType and Retro68's signed LONGINT. */
#define MACSURF_GESTALT_SYSVER        0x73797376L  /* 'sysv'                 */
#define MACSURF_GESTALT_SYSVER_MAJOR  0x73797331L  /* 'sys1', Mac OS X 10.4+ */
#define MACSURF_GESTALT_SYSVER_MINOR  0x73797332L  /* 'sys2', Mac OS X 10.4+ */
#define MACSURF_GESTALT_SYSVER_BUGFIX 0x73797333L  /* 'sys3', Mac OS X 10.4+ */
#define MACSURF_GESTALT_CARBON        0x63626F6EL  /* 'cbon'                 */

/* File-statics set by osver_probe(). Defaults describe Mac OS 9 -- the
 * primary shipped platform -- so a Gestalt failure degrades to today's
 * behaviour rather than to something new. */
static int  g_osver_ready  = 0;   /* probe has run (idempotence latch)     */
static int  g_osver_logged = 0;   /* RECON OS emitted (one-shot latch)     */
static int  g_os_is_osx    = 0;   /* fail-safe: assume Classic             */
static int  g_os_major     = 9;
static int  g_os_minor     = 0;
static int  g_os_bugfix    = 0;
static long g_os_raw       = 0;   /* 0 = Gestalt could not answer          */
static long g_os_carbon    = 0;   /* raw 'cbon', logged but not interpreted */
static int  g_os_sysv_err  = 0;
static int  g_os_mmb_err   = 0;

/* Silent, idempotent, dependency-free. Safe to call from any accessor. */
static void osver_probe(void)
{
#ifdef __MACOS9__
	/* SInt32, NOT long: UI 3.4 declares Gestalt(OSType, long *) while
	 * Retro68 declares Gestalt(OSType, LONGINT *). SInt32 is the only
	 * spelling that satisfies both -- macsurf_memory.c:145 uses long and
	 * the Linux gate flags it as an incompatible pointer type. */
	SInt32 v;
	SInt32 maj;
	SInt32 mnr;
	SInt32 bug;
	SInt32 cbv;
	OSErr  err;
#endif

	if (g_osver_ready) return;
	g_osver_ready = 1;

#ifdef __MACOS9__
	v = 0;
	err = Gestalt((OSType)MACSURF_GESTALT_SYSVER, &v);
	g_os_sysv_err = (int)err;
	if (err == noErr && v != 0) {
		g_os_raw = (long)v;
		/* Packed BCD. The MAJOR field is TWO BCD digits, so 0x10 means
		 * ten, not sixteen: 0x0922 -> 9.2.2, 0x1039 -> 10.3.9. */
		g_os_major  = (int)((((v >> 12) & 0x0F) * 10) + ((v >> 8) & 0x0F));
		g_os_minor  = (int)((v >> 4) & 0x0F);
		g_os_bugfix = (int)(v & 0x0F);
		g_os_is_osx = (v >= 0x1000) ? 1 : 0;
	}

	/* 10.4+ integer selectors. These fail cleanly with
	 * gestaltUndefSelectorErr (-5551) on OS 9 AND on 10.0-10.3, where the
	 * BCD decode above is already exact -- so a non-zero g_os_mmb_err is
	 * EXPECTED on the 10.3 test machine and is not a defect.
	 *
	 * Deliberately one-way: this block may CONFIRM OS X but can never clear
	 * g_os_is_osx, so a surprising answer cannot flip an OS 9 machine. */
	maj = 0;
	mnr = 0;
	bug = 0;
	err = Gestalt((OSType)MACSURF_GESTALT_SYSVER_MAJOR, &maj);
	g_os_mmb_err = (int)err;
	if (err == noErr && maj > 0) {
		if (Gestalt((OSType)MACSURF_GESTALT_SYSVER_MINOR, &mnr) != noErr)
			mnr = 0;
		if (Gestalt((OSType)MACSURF_GESTALT_SYSVER_BUGFIX, &bug) != noErr)
			bug = 0;
		g_os_major  = (int)maj;
		g_os_minor  = (int)mnr;
		g_os_bugfix = (int)bug;
		if (maj >= 10) g_os_is_osx = 1;
	}

	cbv = 0;
	if (Gestalt((OSType)MACSURF_GESTALT_CARBON, &cbv) == noErr)
		g_os_carbon = (long)cbv;
#else
	/* Linux syntax-check build: no Toolbox to ask. The statics keep their
	 * Mac OS 9 defaults so any caller sees the primary target's behaviour. */
#endif
}

void macsurf_osver_init(void)
{
	osver_probe();

	if (g_osver_logged) return;
	g_osver_logged = 1;

#ifdef __MACOS9__
	/* "RECON OS" MUST be present in macsurf_log_is_crash_report() over in
	 * macsurf_debug_log.c or this line is silently dropped in a shipped
	 * build -- that gate matches LITERAL substrings, not a "RECON" prefix.
	 * %p is the only hex specifier macsurf_debug_log_writef has, so the
	 * packed BCD prints the way a human reads it (e.g. 00001039). */
	macsurf_debug_log_writef(
		"RECON OS raw=%p ver=%d.%d.%d osx=%d carbon=%p sysverr=%d mmerr=%d",
		(void *)g_os_raw, g_os_major, g_os_minor, g_os_bugfix,
		g_os_is_osx, (void *)g_os_carbon, g_os_sysv_err, g_os_mmb_err);

	if (g_os_raw == 0) {
		/* Carries "FAIL", so it survives the gate with no entry of its own. */
		macsurf_debug_log_writef(
			"RECON OS gestalt FAIL err=%d -- assuming Mac OS 9",
			g_os_sysv_err);
	}
	macsurf_debug_log_flush();
#endif
}

int macsurf_os_is_osx(void)
{
	osver_probe();
	return g_os_is_osx;
}

int macsurf_os_major(void)
{
	osver_probe();
	return g_os_major;
}

int macsurf_os_minor(void)
{
	osver_probe();
	return g_os_minor;
}

int macsurf_os_bugfix(void)
{
	osver_probe();
	return g_os_bugfix;
}

long macsurf_os_raw(void)
{
	osver_probe();
	return g_os_raw;
}
