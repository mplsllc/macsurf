/*
 * ppc_mul64_probe.h
 *
 * Stage A.5 PowerPC 64-bit multiply probe.
 *
 * Verifies that CW8 PPC correctly emits the 32x32->64 multiply pattern
 * used by BearSSL's i31_moddiv.c (and other i31 family files we intend
 * to compile). The known MacSurf hazard is the "shift-multiply by
 * power-of-two constant" miscompile documented in CLAUDE.md "CW8 PPC
 * miscompiles long long multiply-by-constant" gotcha; this probe
 * answers whether the i31 family's *variable*variable* multiply is
 * also affected.
 *
 * Build matrix (all on the dev Mac):
 *   - CW8 no optimization
 *   - CW8 size optimization  (-Os equivalent in the IDE)
 *   - CW8 speed optimization (-O4 equivalent)
 * Targets:
 *   - G3 hardware
 *   - G4 hardware
 *   - SheepShaver (emulator; informational only -- not authoritative)
 *
 * Outputs a per-test pass/fail count via MS_LOG and the file-backed
 * MacSurf Debug.log. Non-destructive; no networking, no Toolbox state
 * mutation beyond log writes.
 *
 * If ANY combination fails: do not patch i31_moddiv.c with the
 * libcss-style int64 -> double workaround. Floating point is
 * non-constant-time on PPC and would defeat BearSSL's constant-time
 * design. The correct fix is PPC inline asm using mullw + mulhwu,
 * gated by #ifdef __MWERKS__ inside i31_moddiv.c (one of the few
 * upstream patches we'd accept).
 */

#ifndef PPC_MUL64_PROBE_H
#define PPC_MUL64_PROBE_H

#ifdef __MWERKS__
#include <Types.h>
#else
typedef short OSErr;
#define noErr 0
#endif

/*
 * Run the full probe matrix. Returns:
 *   0    all passed
 *   >0   number of failed patterns (1..N)
 *
 * Logs each failure case to MacSurf Debug.log via macsurf_debug_log_writef
 * if MS_LOG is available; otherwise prints to stderr/stdout on a host
 * build.
 */
int PPC_Mul64_RunProbe(void);

#endif /* PPC_MUL64_PROBE_H */
