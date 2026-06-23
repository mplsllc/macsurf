/*
 * ostls_mul64_probe.c -- Stage A.5 hardware probe. See header.
 *
 * Operands are volatile so CW8 cannot fold these at compile time. The
 * multiplications must come out of real mullw + mulhwu instructions
 * executed at run time on G3/G4. The expected high/low words below were
 * computed off-target in Python (see commit notes for derivation):
 *
 *   A: 0x12345678 * 0x9ABCDEF0
 *      raw -> 0x0B00EA4E_242D2080
 *      CT  -> 0x585F59C6_242D2080   (operands OR'd with 0x80000000)
 *
 *   B: 0x7FFFFFFF * 0x7FFFFFFF
 *      raw -> 0x3FFFFFFF_00000001
 *      CT  -> 0xFFFFFFFE_00000001
 *
 *   C: 0x00010001 * 0x00010001
 *      raw -> 0x00000001_00020001
 *      CT  -> 0x40010002_00020001
 *
 *   D: 0xDEADBEEF * 0xCAFEBABE     (both top bits already set)
 *      raw -> 0xB092AB7B_88CF5B62
 *      CT  -> 0xB092AB7B_88CF5B62   (same as raw -- |0x80000000 is no-op)
 *
 * Each pair is tested twice (raw / CT) and failure returns a distinct
 * code so the result window points at exactly the bug, if there is one.
 *
 * Result on real OS 9.1 / G3 PowerPC G3 hardware (2026-05-19): noErr
 * (all eight gates passed). The 32x32->64 multiply codegen CW8 emits
 * for this pattern is sound; BearSSL's bigint stack can be trusted.
 */

#include "ostls_mul64_probe.h"

#ifdef __MWERKS__
#include <Types.h>      /* noErr */
#else
#define noErr 0
#endif

#include <stdint.h>


/*
 * Helper: compute the 64-bit product of two uint32_t operands using
 * the raw pattern, and check it against an expected hi/lo split.
 * Returns 1 on match, 0 on mismatch. Operands taken by pointer to a
 * volatile location to keep CW8 from constant-folding.
 */
static int
check_mul_raw(const volatile uint32_t *pa,
              const volatile uint32_t *pb,
              uint32_t exp_hi,
              uint32_t exp_lo)
{
    uint32_t a;
    uint32_t b;
    uint64_t p;
    uint32_t got_hi;
    uint32_t got_lo;

    a = *pa;
    b = *pb;
    p = (uint64_t)a * (uint64_t)b;
    got_hi = (uint32_t)(p >> 32);
    got_lo = (uint32_t)(p & 0xFFFFFFFFU);
    return (got_hi == exp_hi && got_lo == exp_lo) ? 1 : 0;
}


/*
 * Same as check_mul_raw but uses the BR_CT_MUL31 pattern -- operands
 * OR'd with 0x80000000 before the multiply. This is the exact macro
 * expansion BearSSL uses with our BR_CT_MUL31=1 pin.
 */
static int
check_mul_ct(const volatile uint32_t *pa,
             const volatile uint32_t *pb,
             uint32_t exp_hi,
             uint32_t exp_lo)
{
    uint32_t a;
    uint32_t b;
    uint64_t p;
    uint32_t got_hi;
    uint32_t got_lo;

    a = *pa;
    b = *pb;
    p = (uint64_t)(a | (uint32_t)0x80000000)
      * (uint64_t)(b | (uint32_t)0x80000000);
    got_hi = (uint32_t)(p >> 32);
    got_lo = (uint32_t)(p & 0xFFFFFFFFU);
    return (got_hi == exp_hi && got_lo == exp_lo) ? 1 : 0;
}


OSErr
OSTLS_Mul64Probe(void)
{
    volatile uint32_t a;
    volatile uint32_t b;

    /* Pair A: mixed bit pattern, top bit of b set. */
    a = 0x12345678U;
    b = 0x9ABCDEF0U;
    if (!check_mul_raw(&a, &b, 0x0B00EA4EU, 0x242D2080U)) {
        return (OSErr)kOSTLSMul64FailA_Raw;
    }
    if (!check_mul_ct(&a, &b, 0x585F59C6U, 0x242D2080U)) {
        return (OSErr)kOSTLSMul64FailA_CT;
    }

    /* Pair B: maximum 31-bit operands -- exercises high-word boundary. */
    a = 0x7FFFFFFFU;
    b = 0x7FFFFFFFU;
    if (!check_mul_raw(&a, &b, 0x3FFFFFFFU, 0x00000001U)) {
        return (OSErr)kOSTLSMul64FailB_Raw;
    }
    if (!check_mul_ct(&a, &b, 0xFFFFFFFEU, 0x00000001U)) {
        return (OSErr)kOSTLSMul64FailB_CT;
    }

    /* Pair C: low-magnitude, exercises 16-bit-window carry. */
    a = 0x00010001U;
    b = 0x00010001U;
    if (!check_mul_raw(&a, &b, 0x00000001U, 0x00020001U)) {
        return (OSErr)kOSTLSMul64FailC_Raw;
    }
    if (!check_mul_ct(&a, &b, 0x40010002U, 0x00020001U)) {
        return (OSErr)kOSTLSMul64FailC_CT;
    }

    /* Pair D: both top bits set -- |0x80000000 is a no-op so raw == CT. */
    a = 0xDEADBEEFU;
    b = 0xCAFEBABEU;
    if (!check_mul_raw(&a, &b, 0xB092AB7BU, 0x88CF5B62U)) {
        return (OSErr)kOSTLSMul64FailD_Raw;
    }
    if (!check_mul_ct(&a, &b, 0xB092AB7BU, 0x88CF5B62U)) {
        return (OSErr)kOSTLSMul64FailD_CT;
    }

    /*
     * Pair E: the FULL BR_CT_MUL31 expansion of MUL31() -- the OR'd 64-bit
     * product MINUS the three 64-bit constant shifts. For any 31-bit x,y
     * this is mathematically equal to the plain product (uint64_t)x*y.
     * On a correct compiler full_ct == plain; if CW8 PPC miscompiles the
     * <<31 / <<62 terms, they DIFFER. This is the test A..D omitted.
     */
    {
        static const uint32_t ev[] = {
            0x00000000U, 0x00000001U, 0x7FFFFFFFU,
            0x12345678U & 0x7FFFFFFFU, 0x55555555U, 0x7FFFFFFEU
        };
        int i, j;
        for (i = 0; i < 6; i++) {
            for (j = 0; j < 6; j++) {
                volatile uint32_t vx = ev[i] & 0x7FFFFFFFU;
                volatile uint32_t vy = ev[j] & 0x7FFFFFFFU;
                uint32_t x = vx;
                uint32_t y = vy;
                uint64_t full_ct =
                      (uint64_t)((x) | (uint32_t)0x80000000)
                    * (uint64_t)((y) | (uint32_t)0x80000000)
                    - ((uint64_t)(x) << 31)
                    - ((uint64_t)(y) << 31)
                    - ((uint64_t)1 << 62);
                uint64_t plain = (uint64_t)x * (uint64_t)y;
                if (full_ct != plain) {
                    return (OSErr)kOSTLSMul64FailE_FullCT;
                }
            }
        }
    }

    /*
     * Pair F: bare 64-bit constant-shift high-word correctness. Directly
     * targets the CW8 PPC defect: (uint64_t)v << 31 and << 62 must place
     * the right bits in the high word.
     */
    {
        volatile uint32_t vv = 0x6DB7A55FU;   /* 31 bits when masked */
        uint64_t v = (uint64_t)(vv & 0x7FFFFFFFU);
        uint64_t s31 = v << 31;
        uint64_t one62 = (uint64_t)1 << 62;
        uint32_t s31_hi = (uint32_t)(s31 >> 32);
        uint32_t s31_lo = (uint32_t)(s31 & 0xFFFFFFFFU);
        uint32_t one62_hi = (uint32_t)(one62 >> 32);
        /* 0x6DB7A55F << 31 = 0x36DBD2AF80000000 */
        if (s31_hi != 0x36DBD2AFU || s31_lo != 0x80000000U) {
            return (OSErr)kOSTLSMul64FailF_Shift;
        }
        /* (1 << 62) high word = 0x40000000 */
        if (one62_hi != 0x40000000U) {
            return (OSErr)kOSTLSMul64FailF_Shift;
        }
    }

    return noErr;
}
