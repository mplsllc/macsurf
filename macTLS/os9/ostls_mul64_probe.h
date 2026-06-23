/*
 * ostls_mul64_probe.h
 *
 * Stage A.5 hardware probe -- confirms that CW8 PPC codegen for the
 * uint32_t * uint32_t -> uint64_t pattern (PPC `mullw` + `mulhwu`)
 * produces correct results on real G3/G4 hardware.
 *
 * BearSSL's bigint kernel (MUL31 in bearssl/src/inner.h) compiles down
 * to exactly that pattern. The known-bad CW8 PPC miscompile is the
 * DIFFERENT pattern `(long long)a * small_const` (see CLAUDE.md "CW8
 * PPC long long codegen is broken") which we already side-stepped by
 * pinning BR_64=0 in ostls_cw8_prefix.h. This probe confirms that the
 * 32x32->64 path BearSSL DOES use is in fact correct, before Stage B
 * lets cert validation depend on it.
 *
 * Returns noErr on pass. On fail, returns a code in the 200..220 range
 * identifying which pair and pattern broke (see kOSTLSMul64* below).
 * The smoke-test result window in MacTLSTest/main.c maps each code to
 * a human-readable label.
 */

#ifndef OSTLS_MUL64_PROBE_H
#define OSTLS_MUL64_PROBE_H

#ifdef __MWERKS__
#include <Types.h>      /* OSErr */
#else
typedef short OSErr;
#endif

/*
 * Mul64 probe result codes. Disjoint from kOSTLSSmoke* (100..103).
 *
 * Pair A: 0x12345678 * 0x9ABCDEF0  -- mixed bit pattern, top bit of b set
 * Pair B: 0x7FFFFFFF * 0x7FFFFFFF  -- maximum 31-bit operands
 * Pair C: 0x00010001 * 0x00010001  -- low-magnitude carry probe
 * Pair D: 0xDEADBEEF * 0xCAFEBABE  -- maximum-magnitude, both top bits set
 *
 * "raw" pattern: (uint64_t)a * (uint64_t)b
 * "CT"  pattern: (uint64_t)(a|0x80000000) * (uint64_t)(b|0x80000000)
 *                (matches BR_CT_MUL31 expansion in inner.h)
 */
enum {
    kOSTLSMul64OK            = 0,
    kOSTLSMul64FailA_Raw     = 200,
    kOSTLSMul64FailA_CT      = 201,
    kOSTLSMul64FailB_Raw     = 202,
    kOSTLSMul64FailB_CT      = 203,
    kOSTLSMul64FailC_Raw     = 204,
    kOSTLSMul64FailC_CT      = 205,
    kOSTLSMul64FailD_Raw     = 206,
    kOSTLSMul64FailD_CT      = 207,
    /*
     * Pair E/F probe the part the A..D pairs MISSED: the FULL BR_CT_MUL31
     * expansion of MUL31(), which is the OR'd 64-bit product MINUS three
     * 64-bit constant shifts ((x<<31), (y<<31), (1<<62)). The original A.5
     * probe only checked the multiply, not the shift-subtraction, which is
     * the exact CW8 PPC 64-bit-shift-by-constant defect. If E or F fails on
     * hardware, BR_CT_MUL31 MUST stay 0 (the shipped fix); the plain
     * multiply path is correct and is what the build now uses.
     *   E: full MUL31 macro result == plain (uint64_t)x*y, several operands
     *   F: bare (uint64_t)v << 31 / << 62 high-word correctness
     */
    kOSTLSMul64FailE_FullCT  = 208,
    kOSTLSMul64FailF_Shift   = 209
};

/*
 * Run the probe. Returns kOSTLSMul64OK (0) on full pass. Idempotent.
 * Stack-only, no globals, no allocator.
 */
OSErr OSTLS_Mul64Probe(void);

#endif /* OSTLS_MUL64_PROBE_H */
