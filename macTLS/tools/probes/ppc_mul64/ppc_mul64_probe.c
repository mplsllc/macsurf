/*
 * ppc_mul64_probe.c -- Stage A.5 CW8 PPC 64-bit multiply probe.
 *
 * See ppc_mul64_probe.h for design rationale. This file builds under
 * both CW8 (where it tests real PPC codegen) and Linux/GCC (where it
 * verifies the schoolbook reference implementation against the
 * platform's native multiplier as a sanity check on the probe itself).
 *
 * The probe runs three pattern families:
 *
 *   FAMILY A -- straight 32x32->64 multiply
 *     uint64_t z = (uint64_t)a * (uint64_t)b;
 *     This is the pattern i31_moddiv.c line 155 uses.
 *
 *   FAMILY B -- multiply-accumulate, two-operand sum
 *     uint64_t z = (uint64_t)a * b + (uint64_t)c * d;
 *     This is the pattern i31_moddiv.c lines 155-156 use.
 *
 *   FAMILY C -- shift after multiply
 *     uint64_t z = (uint64_t)a * b;
 *     uint32_t hi = (uint32_t)(z >> 32);
 *     This is the carry-extraction pattern at line 168.
 *
 * For each family we run a small hand-picked vector of inputs that
 * exercises edge cases:
 *   - both zero (degenerate)
 *   - 1 * 1 (degenerate)
 *   - small * large
 *   - large * large (full 64-bit result)
 *   - 0xFFFFFFFF * 0xFFFFFFFF (max product)
 *   - random-looking pairs that the original libcss miscompile happened
 *     to expose: 131072 * 1024 (= 2^17 * 2^10 = 2^27 = 134217728).
 *
 * The reference implementation uses *only* 32-bit operations and is
 * therefore immune to the 64-bit codegen bug:
 *
 *   schoolbook_mul32x32_to_64(a, b, &hi, &lo)
 *     ah = a >> 16, al = a & 0xFFFF
 *     bh = b >> 16, bl = b & 0xFFFF
 *     ll = al * bl
 *     lh = al * bh
 *     hl = ah * bl
 *     hh = ah * bh
 *     mid = lh + hl + (ll >> 16)  [carry-aware split shown below]
 *     lo = (mid << 16) | (ll & 0xFFFF)
 *     hi = hh + (mid >> 16) + (carries)
 */

#include "ppc_mul64_probe.h"

#ifdef __MWERKS__

#include "macsurf_debug_log.h"
#define PROBE_LOG(msg)        MS_LOG(msg)
/*
 * CW8 supports __VA_ARGS__ in macros as a pre-C99 extension; route the
 * formatted variant straight to macsurf_debug_log_writef (a real C
 * varargs function), preserving the file-backed log channel.
 */
#define PROBE_LOGF(args)      macsurf_debug_log_writef args

#else

/*
 * Host build (Linux sanity check via x86-64 gcc, or Retro68 PPC GCC
 * pre-flight). Use a varargs C function rather than a variadic macro so
 * `-std=c89 -pedantic-errors` accepts the file. The CW8 path above uses
 * __VA_ARGS__ in macros, which CW8 supports as a pre-C99 extension.
 */
#include <stdio.h>
#include <stdarg.h>
static void
probe_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
#define PROBE_LOG(msg)        do { puts(msg); } while (0)
#define PROBE_LOGF(args)      probe_logf args

#endif

#ifdef __MWERKS__
typedef unsigned long      ProbeU32;
typedef unsigned long long ProbeU64;
#else
#include <stdint.h>
typedef uint32_t ProbeU32;
typedef uint64_t ProbeU64;
#endif


/* ===================================================================== */
/* Schoolbook 32x32 -> 64 using only 32-bit operations.                  */
/* ===================================================================== */

/*
 * Compute (a * b) splitting into hi:lo. Uses only 32-bit multiplies,
 * 32-bit additions, and 32-bit shifts. This is the reference against
 * which we compare CW8's native 64-bit multiply.
 *
 * Carry handling:
 *   ll = al * bl    (max 0xFFFE0001, fits in 32 bits)
 *   lh = al * bh    (max 0xFFFE0001)
 *   hl = ah * bl    (max 0xFFFE0001)
 *   hh = ah * bh    (max 0xFFFE0001)
 *
 *   lower_mid_sum = lh + hl   may overflow 32 bits if both are large.
 *   Track carry into the high half explicitly.
 */
static void
schoolbook_mul32x32(ProbeU32 a, ProbeU32 b, ProbeU32 *hi_out, ProbeU32 *lo_out)
{
    ProbeU32 al = a & 0xFFFFu;
    ProbeU32 ah = a >> 16;
    ProbeU32 bl = b & 0xFFFFu;
    ProbeU32 bh = b >> 16;

    ProbeU32 ll = al * bl;
    ProbeU32 lh = al * bh;
    ProbeU32 hl = ah * bl;
    ProbeU32 hh = ah * bh;

    ProbeU32 mid;
    ProbeU32 mid_carry;
    ProbeU32 lo;
    ProbeU32 hi;

    /* mid = lh + hl. May overflow 32 bits; capture carry. */
    mid = lh + hl;
    mid_carry = (mid < lh) ? 1u : 0u;  /* 1 if addition wrapped */

    /* Combine the high half of ll with mid. */
    {
        ProbeU32 ll_hi = ll >> 16;
        ProbeU32 mid_plus_ll_hi = mid + ll_hi;
        if (mid_plus_ll_hi < mid) {
            mid_carry++;
        }
        mid = mid_plus_ll_hi;
    }

    /* lo = (mid_low << 16) | ll_low */
    lo = (mid << 16) | (ll & 0xFFFFu);

    /* hi = hh + mid_high + (mid_carry << 16) */
    hi = hh + (mid >> 16) + (mid_carry << 16);

    *hi_out = hi;
    *lo_out = lo;
}


/* ===================================================================== */
/* Test vectors                                                          */
/* ===================================================================== */

struct mul_case {
    ProbeU32 a;
    ProbeU32 b;
    const char *label;
};

static const struct mul_case kCases[] = {
    {          0u,          0u, "0 * 0" },
    {          1u,          1u, "1 * 1" },
    {          1u, 0xFFFFFFFFu, "1 * max32" },
    { 0xFFFFFFFFu,          1u, "max32 * 1" },
    { 0xFFFFFFFFu, 0xFFFFFFFFu, "max32 * max32" },
    {     131072u,       1024u, "131072 * 1024 (libcss miscompile case)" },
    {       1024u,     131072u, "1024 * 131072 (reverse)" },
    { 0x12345678u, 0x87654321u, "mid mid" },
    { 0xDEADBEEFu, 0xCAFEBABEu, "random a" },
    { 0xFFFFu,     0xFFFFu,     "16x16 (no high half)" },
    { 0x80000000u, 0x00000002u, "boundary shift case" },
    { 0xAAAAAAAAu, 0x55555555u, "alternating bits" }
};
static const unsigned int kNumCases =
    (unsigned int)(sizeof kCases / sizeof kCases[0]);


/* ===================================================================== */
/* Family A: straight 32x32 -> 64                                         */
/* ===================================================================== */

static int
run_family_a(void)
{
    unsigned int i;
    int fails = 0;
    PROBE_LOG("probe A: (u64)a * (u64)b");

    for (i = 0; i < kNumCases; i++) {
        ProbeU32 a = kCases[i].a;
        ProbeU32 b = kCases[i].b;
        ProbeU64 z;
        ProbeU32 native_hi, native_lo;
        ProbeU32 ref_hi, ref_lo;

        z = (ProbeU64)a * (ProbeU64)b;
        native_hi = (ProbeU32)(z >> 32);
        native_lo = (ProbeU32)(z & 0xFFFFFFFFu);

        schoolbook_mul32x32(a, b, &ref_hi, &ref_lo);

        if (native_hi != ref_hi || native_lo != ref_lo) {
            fails++;
            PROBE_LOGF(("FAIL A %s a=%lx b=%lx",
                kCases[i].label,
                (unsigned long)a, (unsigned long)b));
            PROBE_LOGF(("  native hi=%lx lo=%lx",
                (unsigned long)native_hi, (unsigned long)native_lo));
            PROBE_LOGF(("  ref    hi=%lx lo=%lx",
                (unsigned long)ref_hi, (unsigned long)ref_lo));
        }
    }
    return fails;
}


/* ===================================================================== */
/* Family B: two-product sum (i31_moddiv pattern)                         */
/* ===================================================================== */

static int
run_family_b(void)
{
    /* Use pairs from kCases for (a, b) and shifted pairs for (c, d) so
     * the sum exercises the carry between products. */
    unsigned int i;
    int fails = 0;
    PROBE_LOG("probe B: a*b + c*d (two-product sum)");

    for (i = 0; i < kNumCases; i++) {
        ProbeU32 a = kCases[i].a;
        ProbeU32 b = kCases[i].b;
        ProbeU32 c = kCases[(i + 3) % kNumCases].a;
        ProbeU32 d = kCases[(i + 5) % kNumCases].b;

        ProbeU64 native;
        ProbeU64 ref;
        ProbeU32 ab_hi, ab_lo, cd_hi, cd_lo;
        ProbeU32 lo, hi, carry;

        native = (ProbeU64)a * (ProbeU64)b + (ProbeU64)c * (ProbeU64)d;

        /* Reference: schoolbook each product, then 64-bit add via
         * 32-bit pieces with carry. */
        schoolbook_mul32x32(a, b, &ab_hi, &ab_lo);
        schoolbook_mul32x32(c, d, &cd_hi, &cd_lo);
        lo = ab_lo + cd_lo;
        carry = (lo < ab_lo) ? 1u : 0u;
        hi = ab_hi + cd_hi + carry;
        ref = ((ProbeU64)hi << 32) | lo;

        if (native != ref) {
            fails++;
            PROBE_LOGF(("FAIL B i=%u native_hi=%lx native_lo=%lx",
                i,
                (unsigned long)(ProbeU32)(native >> 32),
                (unsigned long)(ProbeU32)native));
            PROBE_LOGF(("       ref    hi=%lx       lo=%lx",
                (unsigned long)hi, (unsigned long)lo));
        }
    }
    return fails;
}


/* ===================================================================== */
/* Family C: shift after multiply (carry extraction)                      */
/* ===================================================================== */

static int
run_family_c(void)
{
    unsigned int i;
    int fails = 0;
    PROBE_LOG("probe C: (a*b) >> 32 (carry extraction)");

    for (i = 0; i < kNumCases; i++) {
        ProbeU32 a = kCases[i].a;
        ProbeU32 b = kCases[i].b;
        ProbeU64 z;
        ProbeU32 native_hi;
        ProbeU32 ref_hi, ref_lo;

        z = (ProbeU64)a * (ProbeU64)b;
        native_hi = (ProbeU32)(z >> 32);

        schoolbook_mul32x32(a, b, &ref_hi, &ref_lo);

        if (native_hi != ref_hi) {
            fails++;
            PROBE_LOGF(("FAIL C %s a=%lx b=%lx native_hi=%lx ref_hi=%lx",
                kCases[i].label,
                (unsigned long)a, (unsigned long)b,
                (unsigned long)native_hi,
                (unsigned long)ref_hi));
        }
    }
    return fails;
}


/* ===================================================================== */
/* Driver                                                                 */
/* ===================================================================== */

int
PPC_Mul64_RunProbe(void)
{
    int a = run_family_a();
    int b = run_family_b();
    int c = run_family_c();
    int total = a + b + c;

    if (total == 0) {
        PROBE_LOG("ppc_mul64 probe: ALL PASS");
    } else {
        PROBE_LOGF(("ppc_mul64 probe: %d FAIL (A=%d B=%d C=%d)",
            total, a, b, c));
    }
    return total;
}


/* ===================================================================== */
/* Optional standalone main() for host build                              */
/* ===================================================================== */

#ifndef __MWERKS__
#ifndef PPC_MUL64_NO_MAIN
int
main(void)
{
    int rc = PPC_Mul64_RunProbe();
    return (rc == 0) ? 0 : 1;
}
#endif
#endif
