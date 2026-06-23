# Stage A.5 — CW8 PPC 64-bit multiply probe

Hardware probe that answers one question: **does CW8 PPC correctly emit
`(uint64_t)a * (uint64_t)b` for the 32×32→64 pattern that
`bearssl/src/int/i31_moddiv.c` uses?**

If yes → leave i31_moddiv.c untouched, ship as-is.

If no → upstream patch with a `#ifdef __MWERKS__` PPC inline-asm path
(`mullw` + `mulhwu`). **Do NOT** apply the libcss-style
`int64 → double` workaround: floating-point on PPC is non-constant-time
and would defeat BearSSL's CT design.

## How to run

Run it twice: once on Linux (sanity check that the probe itself is
correct), once on real OS 9 hardware in CW8 (the actual test).

### Linux baseline

```sh
cd /home/patrick/Webs/macsurf/macTLS
gcc -std=c89 -pedantic-errors -Wall -Wextra -Wno-long-long \
    -o /tmp/probe_host tools/probes/ppc_mul64/ppc_mul64_probe.c
/tmp/probe_host
```

Expected output:

```
probe A: (u64)a * (u64)b
probe B: a*b + c*d (two-product sum)
probe C: (a*b) >> 32 (carry extraction)
ppc_mul64 probe: ALL PASS
```

A failure here means the *schoolbook reference* implementation in
`ppc_mul64_probe.c` is wrong — fix it before trusting the CW8 run.
The probe has been verified to pass on x86-64 Linux at write time.

### CW8 on real hardware

This is the actual test. There is no Linux substitute.

1. Copy `tools/probes/ppc_mul64/ppc_mul64_probe.c` and
   `ppc_mul64_probe.h` to the Mac alongside the rest of the macTLS tree.

2. Add the two files to a **dedicated** CW8 project (NOT MacSurf.mcp).
   The probe should be tested in isolation so the rest of MacSurf's
   build environment can't influence the codegen. A bare Carbon
   console-style project is fine.

3. The dedicated project's prefix must include `macsurf_debug_log.h`
   (so `MS_LOG` works) or you can patch the `__MWERKS__` branch in
   the probe to use `DebugStr()` / `SysBeep()` if a console build is
   easier. Keep the schoolbook math untouched.

4. Call `PPC_Mul64_RunProbe()` once from `main()` and write the
   result to a file or display it on the console.

5. Run the matrix: **three optimization levels × two CPUs**:

   | Optimization | G3 (OS 9.1) | G4 (OS 9.2.2) |
   |---|---|---|
   | No optimization (default debug) | ☐ | ☐ |
   | Size optimization (project setting "Smaller") | ☐ | ☐ |
   | Speed optimization (project setting "Faster") | ☐ | ☐ |

   SheepShaver result is informational only — emulators are more
   forgiving than real hardware on codegen edge cases.

## Interpreting the result

| Result | Meaning | Action |
|---|---|---|
| `ALL PASS` on every cell | CW8 PPC 64-bit codegen is trustworthy for the i31 multiply pattern | Ship i31_moddiv.c unchanged. Stage A passes. |
| One or more cells fail | CW8 miscompiles the pattern at that optimization × CPU | Patch i31_moddiv.c with `#ifdef __MWERKS__` PPC asm; document in CLAUDE.md |
| Compile error | Probe itself is wrong or environment is broken | Fix probe first, then re-run |
| Crash | Codegen is so wrong it produces invalid instructions | Definite patch needed; capture MacsBug stack if possible |

## What the probe does NOT cover

This probe is **necessary but not sufficient**. Even if all 12 test
cases pass, BearSSL exercises 64-bit multiplies under more pressure than
the probe simulates:

- Long sequences of multiply-accumulate (i31_modpow2's inner loop).
- Mixed with shift, AND, and XOR on the same 64-bit values.
- Allocated on the stack across hundreds of recursive crypto calls.
- Compiled with whatever optimizer pass orderings BearSSL's headers
  trigger via `static inline`.

If the probe passes but the smoke test still misbehaves once Stage B
brings up real handshakes, **suspect 64-bit codegen again** even though
the probe was clean. The probe is a fast first filter, not a proof.

## Why these specific test vectors

The 12 vectors are chosen to cover:

1. **Degenerate cases** (0×0, 1×1, max×1) — exercise zero/no-carry paths.
2. **Maximum product** (0xFFFFFFFF × 0xFFFFFFFF = 0xFFFFFFFE00000001) — exercises the full 64-bit range and ensures the carry from `lh+hl` propagates correctly.
3. **The libcss miscompile case** (131072 × 1024 = 134217728) — the exact pattern that exposed the CW8 PPC shift-multiply bug documented in CLAUDE.md. Note: that bug fired on `(long long)constant * scaled_var`, not `(uint64_t)var * (uint64_t)var` — the probe shows whether the variable-variable case is also affected.
4. **Boundary shift cases** (0x80000000 × 0x00000002 = 0x100000000) — multiplies straddling the 32-bit boundary where the entire low half is zero and the result is purely in the high half.
5. **Random-looking pairs** (0xDEADBEEF × 0xCAFEBABE, etc.) — non-aligned bit patterns that won't accidentally pass via a buggy-but-symmetric codegen.

If any of these fails on real hardware, the failing label in the
output identifies the case directly.
