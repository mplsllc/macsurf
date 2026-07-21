/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macsurf_osver.h — runtime host-OS version detection (OS 9 vs Mac OS X).
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * fixes936 (OS X tier 1) — MacSurf ships ONE Carbon CFM PowerPC binary, and
 * MacSurf.r already carries the 'carb' resource, so the very same fragment
 * launches on Mac OS 8.6/9.x AND on Mac OS X 10.0-10.4 (via LaunchCFMApp).
 * Almost nothing in the frontend needs to know which one it is: QuickDraw,
 * GWorlds, FSSpec file I/O, the PPC time base, Toolbox entropy sources and
 * MixedMode UPPs all behave on both. But a handful of Classic-only Toolbox
 * facts change MEANING under OS X, and a GUARD built on one of them turns
 * into a hard, user-visible failure:
 *
 *   - GetProcessInformation()'s processLocation/processSize describe a
 *     Classic application PARTITION. A Carbon app on OS X is a real BSD
 *     process whose malloc arena the Mach VM places where it likes, so that
 *     window is fiction. macsurf_heap_bounds_init() must NOT narrow its
 *     pointer-validity window to it (see macsurf_memory.c) or valid heap
 *     pointers get false-rejected -- issue #207's blank page, on purpose.
 *   - FreeMem()/MaxBlock() report a Classic zone, not the process.
 *   - Open Transport IS present on Mac OS X 10.0+ (deprecated only at 10.4,
 *     not removed), so the networking stack stays exactly as it is; we log
 *     its health rather than replace it.
 *
 * This module is the ONE place that asks the question, so nothing else has
 * to open-code a Gestalt call.
 *
 * DETECTION. Gestalt(gestaltSystemVersion / 'sysv') has existed since
 * System 7.1 and returns a packed BCD version: 0x0922 = Mac OS 9.2.2,
 * 0x1039 = Mac OS X 10.3.9. It is EXACT through 10.3.9 and SATURATES from
 * 10.4 on (one nibble each for minor and bugfix cannot hold "11"), which is
 * why Mac OS X 10.4 added gestaltSystemVersionMajor/Minor/BugFix
 * ('sys1'/'sys2'/'sys3') returning plain integers. We ask for both and
 * prefer the triple when it answers, so the LOGGED version stays honest on
 * 10.4.11 as well as on 10.3.9.
 *
 * The is-OS-X answer itself NEVER depends on the 10.4-only selectors: every
 * OS X release reports 'sysv' >= 0x1000 and every Classic release reports
 * below it, so the cheap comparison is correct across 10.0 through 10.4.
 * The triple can only CONFIRM OS X, never clear it.
 *
 * FAILURE POLICY. Never returns an error, never blocks startup. If Gestalt
 * cannot answer, the module reports Mac OS 9 -- the primary, shipped,
 * hardware-verified platform -- so an unexplained Gestalt failure can never
 * silently change OS 9 behaviour. The failure is logged on a line carrying
 * "FAIL", which survives the crash-only log gate with no whitelist entry.
 */

#ifndef MACSURF_OSVER_H
#define MACSURF_OSVER_H

/* Probe the host OS and emit the one-shot "RECON OS" line.
 *
 * Call ONCE at startup from main(), AFTER macsurf_debug_log_init() (this
 * logs) and BEFORE macsurf_heap_bounds_init() (the first consumer of the
 * answer). Idempotent: a second call is a no-op.
 *
 * The probe itself is also lazy -- every accessor below runs it on demand --
 * so an early caller still gets a correct answer even if the init ordering
 * in main() is later disturbed. */
void macsurf_osver_init(void);

/* 1 when running on Mac OS X 10.0 or later, 0 on Mac OS 8/9 and 0 when the
 * host OS could not be determined (fail-safe toward the shipped platform). */
int macsurf_os_is_osx(void);

/* Decoded host version. On Mac OS 9.2.2 -> 9 / 2 / 2; on Mac OS X 10.3.9 ->
 * 10 / 3 / 9. Defaults to 9 / 0 / 0 when Gestalt could not answer. */
int macsurf_os_major(void);
int macsurf_os_minor(void);
int macsurf_os_bugfix(void);

/* Raw packed-BCD 'sysv' response (0x0922, 0x1039, ...), or 0 if Gestalt
 * could not answer. Logged verbatim so a surprising decode can be audited
 * against the number the Toolbox actually returned. */
long macsurf_os_raw(void);

#endif /* MACSURF_OSVER_H */
