# BearSSL CW8 / C89 audit (Stage 0)

**Audited tree:** `macTLS/bearssl/` — upstream commit `7bea48e5` (2026-04-06)
**Scanned:** 309 files (294 `.c`, 15 `.h`) — `inc/` + `src/` only
**Tool:** `tools/audit_cw8_compat.py` → `tools/audit_report.json`

## Headline finding

**BearSSL is dramatically closer to C89 than libcss/libdom were.** The audit's
raw aggregate looks worse than it is because three of the largest counts are
either false positives from over-broad regex or are concentrated in files we
will not compile under CW8 anyway. After accounting for those, the actual
patch surface to add BearSSL to `MacSurf.mcp` is small.

Pornin's coding style does not use C99 designated initializers, compound
literals, `//` line comments, for-scope declarations, `restrict`, variadic
macros, `snprintf` / `%zu`, or hex floats. The audit found **zero** of those
across all 309 files. That alone is unusual and very welcome.

## What the raw counts say

| Category              | Raw count | Real signal? | Notes                                                                  |
| --------------------- | --------: | ------------ | ---------------------------------------------------------------------- |
| `uint64_intrinsic`    |       851 | partial      | Concentrated in `_m64`, `_m62`, `_ct64`, `i62` — files we exclude      |
| `inline_kw`           |       180 | yes          | `inline` keyword, mostly `static inline` in 8 headers                  |
| `compound_literal`    |       162 | **no**       | Regex matched `if (...) {` and function bodies. Confirmed 0 real sites |
| `static_inline_hdr`   |       113 | yes          | Subset of `inline_kw`, header-only                                     |
| `posix_header`        |         9 | **no**       | All gated by `BR_USE_URANDOM` / `_WIN32` / `_WIN64` build configs      |
| `union_cast_zero`     |         7 | **no**       | All `~(uint64_t)0` — legitimate C89 cast, not union cast               |
| `flexible_array`      |         5 | **no**       | All `extern const T arr[];` — incomplete-array forward decls, legal    |
| `attribute_gnu`       |         1 | **no**       | `inner.h` `BR_TARGET` macro, gated by `BR_GCC_4_4 \|\| BR_CLANG_3_7`   |
| `builtin_gnu`         |         1 | **no**       | `inner.h` `__builtin_bswap32`, gated by `BR_GCC \|\| BR_CLANG`         |
| `cpp_comment`         |         0 | n/a          | None                                                                   |
| `for_scope_int/ptr`   |         0 | n/a          | None                                                                   |
| `designated_init`     |         0 | n/a          | None                                                                   |
| `variadic_macro`      |         0 | n/a          | None                                                                   |
| `long_long`           |         0 | n/a          | None — but `uint64_t` math is the same hazard                          |
| `restrict_kw`         |         0 | n/a          | None                                                                   |
| `snprintf`            |         0 | n/a          | None                                                                   |
| `size_t_printf`       |         0 | n/a          | None                                                                   |
| `hex_float`           |         0 | n/a          | None                                                                   |
| `forward_enum`        |         0 | n/a          | None                                                                   |

## What's actually real

### 1. `inline` keyword in 8 headers (real, easy to patch)

180 `inline` uses, 113 of them `static inline` in 8 public + internal headers.
CW8 C89 does not accept `inline`.

Worst files:

| File                    | `static inline` |
| ----------------------- | --------------: |
| `inc/bearssl_ssl.h`     |              49 |
| `src/inner.h`           |              42 |
| `inc/bearssl_x509.h`    |              16 |
| `inc/bearssl_hmac.h`    |               3 |
| `inc/bearssl_hash.h`    |               2 |
| `inc/bearssl_pem.h`     |               2 |
| `inc/bearssl_rand.h`    |               1 |
| `inc/bearssl_aead.h`    |               1 |

**Fix strategy:** add `#define inline` (i.e. inline expands to nothing) to a
CW8-only compatibility header included via the prefix. `static inline foo()`
becomes `static foo()`, which is a normal C89 file-local function.

The tradeoff: every TU that includes a BearSSL header that defines a
`static inline` will emit its own private copy of that function. This is
already how the libcss `_i.h` files behave under CW8. Code size grows but
correctness is fine. If a hotspot turns up in profiling later, we convert
those specific functions to macros or move them into `.c` files.

The other 67 `inline` uses are in `.c` files (mostly the 64-bit variants
we exclude — see #2). Whatever remains in the 32-bit `.c` files gets the
same `#define inline` treatment for free.

### 2. 64-bit math is concentrated in files we don't need (real, no patch — exclude)

851 `uint64_t` references is alarming given the CW8 PPC `long long`
miscompile that MacSurf has documented in CLAUDE.md. But almost all of it
sits inside the **64-bit-optimized variants** of code that also has 32-bit
implementations:

| File                              | uint64_t hits | 32-bit equivalent      |
| --------------------------------- | ------------: | ---------------------- |
| `src/ec/ec_p256_m64.c`            |           118 | `ec_p256_m31.c`        |
| `src/ec/ec_c25519_m64.c`          |           104 | `ec_c25519_m31.c`      |
| `src/ec/ec_p256_m62.c`            |            64 | `ec_p256_m31.c`        |
| `src/symcipher/aes_ct64.c`        |            61 | `aes_ct.c`             |
| `src/int/i31_moddiv.c`            |            55 | (stays — see below)    |
| `src/int/i62_modpow2.c`           |            54 | `i31_*` family         |
| `src/symcipher/poly1305_ctmulq.c` |            40 | `poly1305_ctmul.c`     |
| `src/ec/ec_c25519_m62.c`          |            29 | `ec_c25519_m31.c`      |
| `src/hash/ghash_ctmul64.c`        |            27 | `ghash_ctmul.c`        |
| `src/symcipher/aes_ct64_dec.c`    |            20 | `aes_ct.c`             |

**Strategy: exclude the `_m64`, `_m62`, `_ct64`, `_ctmulq`, `_i62` variants
from the CW8 build.** The deep research report already steers performance
work toward `p256_m31` and `c25519_m31` (BearSSL's measured-fast 32-bit
curves on non-AltiVec hardware). Those are the implementations we want
anyway, and they avoid the CW8 PPC 64-bit hazard entirely.

The one exception is `i31_moddiv.c` (55 hits) — that's in the `i31`
big-int family we **do** want. Need to inspect those sites: if they use
`uint64_t` only as intermediate products in a 32×32→64 multiply (the
pattern `(uint64_t)a * (uint64_t)b`), we have two options:

1. **Trust the codegen and ship.** The CW8 miscompile fired specifically
   on `(long long)constant * scaled_var` — a shift-by-power-of-two
   pattern. The textbook 32×32→64 multiply may compile correctly even
   on CW8. Needs a probe round on real hardware.
2. **Replace with a PPC inline-asm `mullw`/`mulhwu` pair** (the inline
   asm sketch in the deep-research report §"Inline PowerPC assembly
   sketch" is exactly the right replacement here). CW8 supports PPC
   inline asm; we'd hide it behind `#ifdef __MWERKS__`.

The deep-research report's `fpmath.h`-style `int64 → double` workaround
that MacSurf uses in libcss is **not appropriate for crypto**. Floating
point is non-constant-time on PPC and would defeat BearSSL's CT design.
Inline asm is the right escape hatch for `i31_moddiv`.

### 3. `inner.h` GCC extensions (real, no patch — already gated)

```c
/* inner.h:272 */
#if BR_GCC_4_4 || BR_CLANG_3_7
#define BR_TARGET(x)   __attribute__((target(x)))
#else
#define BR_TARGET(x)
#endif

/* inner.h:2529 */
#if BR_GCC || BR_CLANG
...
#define br_bswap32   __builtin_bswap32
...
#endif
```

CW8 defines neither `BR_GCC` nor `BR_CLANG`, so both branches expand to the
empty / fallback path. No patch needed.

### 4. `sysrng.c` is the wrong RNG for OS 9 (real, no patch — exclude)

`src/rand/sysrng.c` is the platform entropy bridge for Linux `/dev/urandom`
and Windows `BCryptGenRandom`. We do not compile it. We provide a custom
entropy source (`os9/ostls_entropy.c`) that feeds BearSSL via the
`br_prng_seeder` interface, fed by:

- mouse-position delta hashing
- key-down event timing jitter
- `TickCount` jitter around OT completion notifiers
- a persisted seed file updated on clean shutdown
- a first-run "wiggle the mouse" entropy-gathering dialog

This is the design the deep-research report recommends in §"Entropy and
seed handling" and matches the warning in the mbedTLS-Mac-68k port that
**missing entropy is a serious security issue**.

### 5. False positives confirmed

#### `compound_literal` (162 → 0)

The regex matched any `(expr){` pattern, which fires on every `if (cond) {`,
`while (cond) {`, and function-definition `foo(args)\n{`. A constrained
search for `= (typename){` and `, (typename){` (real compound-literal
positions) finds **zero** sites. BearSSL has no C99 compound literals.

#### `union_cast_zero` (7 → 0)

All seven matches are `~(uint64_t)0` in `src/kdf/shake.c`, plus one
`(__m128i)0` in `src/hash/ghash_pclmul.c` (x86 PCLMUL code we don't
compile). `~(uint64_t)0` is a legal C89 expression: cast literal `0`
to `uint64_t`, then apply bitwise complement.

#### `flexible_array` (5 → 0)

All five matches are `extern const uint32_t br_md5_IV[];` and similar
**incomplete-array forward declarations** in `inner.h` — legal in C89
(the actual definition with a known size lives in the corresponding
`.c` file).

#### `posix_header` (9 → 0)

All gated by `BR_USE_URANDOM` (we don't define) or `_WIN32`/`_WIN64`
(CW8 doesn't define on OS 9). Inert.

## Patch list for v0.1 CW8 build

The actual concrete work to make BearSSL build under CW8:

| # | Action                                                                      | Effort |
|---|-----------------------------------------------------------------------------|--------|
| 1 | Add CW8 compatibility prefix that `#define inline` to empty                 | trivial |
| 2 | Decide which `.c` files to add to `MacSurf.mcp` — exclude 64-bit variants  | one-time |
| 3 | Provide `BR_GCC` / `BR_CLANG` / `_MSC_VER` are all undefined under CW8     | check `inner.h` config |
| 4 | Hand-write `bearssl/src/config.h` for CW8 PPC: `BR_BE_UNALIGNED=0`, `BR_CT_MUL31=1` if profiling demands, no x86 intrinsics | small |
| 5 | Audit `i31_moddiv.c` 64-bit-multiply sites on real PPC hardware (probe)    | one round |
| 6 | Implement `os9/ostls_entropy.c` to satisfy BearSSL's `br_prng_seeder`      | Stage A |
| 7 | Write `OSTLS_SmokeTest()` — init contexts, no network, return ok/fail     | Stage A |

Items 1–4 are the entire Stage 0 patch surface to get a clean compile.
Items 5–7 are Stage A (the smoke test deliverable).

## File-list proposal for `MacSurf.mcp`

The CW8 build should include **only** the 32-bit / portable paths. The
exclusion list:

```
src/ec/ec_p256_m64.c          /* 64-bit P-256, replaced by m31 */
src/ec/ec_p256_m62.c          /* 64-bit P-256 variant, replaced by m31 */
src/ec/ec_c25519_m64.c        /* 64-bit Curve25519, replaced by m31 */
src/ec/ec_c25519_m62.c        /* 64-bit Curve25519 variant */
src/symcipher/aes_ct64.c      /* 64-bit AES constant-time */
src/symcipher/aes_ct64_dec.c
src/symcipher/aes_ct64_enc.c
src/symcipher/poly1305_ctmulq.c
src/int/i62_modpow2.c         /* 62-bit big-int */
src/hash/ghash_ctmul64.c      /* 64-bit GHASH */
src/symcipher/aes_x86ni.c     /* x86 AES-NI */
src/symcipher/aes_x86ni_ctr.c
src/symcipher/aes_x86ni_ctrcbc.c
src/symcipher/aes_x86ni_dec.c
src/symcipher/aes_x86ni_enc.c
src/symcipher/chacha20_sse2.c /* x86 SSE2 */
src/hash/ghash_pclmul.c       /* x86 PCLMUL */
src/hash/ghash_pwr8.c         /* POWER8 vector */
src/symcipher/aes_pwr8.c
src/symcipher/aes_pwr8_ctr.c
src/symcipher/aes_pwr8_ctrcbc.c
src/rand/sysrng.c             /* Linux/Windows entropy — replaced */
```

A separate `tools/make_bearssl_filelist.py` (to be written in Stage A)
will emit the canonical list of remaining `.c` files for the user to
paste into `MacSurf.mcp`.

## Confidence and unknowns

**High confidence:**

- BearSSL is C89-clean as far as the audit can mechanically check.
- The 8 headers' `inline` keyword is the only widespread real fix.
- The 64-bit variants are well-isolated and have 32-bit equivalents.

**Open questions for Stage A:**

- Does CW8 PPC correctly emit `(uint64_t)a * (uint64_t)b` for the 32×32→64
  multiply pattern in `i31_moddiv.c`? Only a real-hardware probe can answer.
- Does `inner.h`'s `BR_LE_UNALIGNED` / `BR_BE_UNALIGNED` detection produce
  the right answer for PPC big-endian? Needs reading the config block.
- What is the actual stack high-water mark of a single TLS 1.2 handshake?
  cryanc's 512 KB warning suggests we need to measure before declaring a
  SIZE resource.

These are Stage A inspection items, not Stage 0 blockers.

---

## Stage A / A.5 closure note (2026-05-18)

Project status as of this commit:

```
Stage 0:                       COMPLETE (audit + vendor)
Stage A (source-side):         COMPLETE (prefix + filelist + entropy + smoketest)
Stage A Mac validation:        PENDING  (smoke test runs on real OS 9 / CW8)
Stage A.5 mul64 validation:    PENDING  (G3 + G4 across 3 optimization levels)
Stage B:                       BLOCKED  until both validations land
```

**Do NOT begin Stage B source work until both gates pass.** The point of
the validation step is to surface any CW8 reality (segment limits, prefix
ordering, optimizer codegen, duplicate symbols, TOC pressure) that
Retro68 pre-flight cannot catch. Building `OSTLSSocket` on top of a
BearSSL tree that merely passes `gcc -fsyntax-only` would defeat the
"isolate each failure boundary" discipline that the staged plan exists
for.

### Architecture pivot (2026-05-18)

macTLS is no longer integrated into MacSurf. It ships as a **standalone
local HTTP proxy Carbon app** that any OS 9 browser can configure as
its HTTP proxy (`127.0.0.1:8765`). Validation now happens in a separate
project `MacTLSTest.mcp` (the bare Carbon harness for Stage A), which
later evolves into the real MacTLS Proxy app over Stages B/C.

Stage 0 + Stage A source-side artifacts unchanged. What pivoted: the
validation target (no longer MacSurf) and Stage B (no longer "wire into
macos9_http_fetcher.c", now "build a TCP listener + proxy parser inside
MacTLS Proxy").

### Mac validation gate

Stage A passes when:

```
1. MacTLSTest.mcp is created in CW8 per MacTLSTest/README.md
2. Project prefix is MacTLSTest/mactlstest_prefix.h
3. Project contains all 253 files from tools/bearssl_cw8_files.txt
   plus os9/ostls_entropy.c, os9/ostls_smoketest.c, MacTLSTest/main.c,
   and MacTLSTest/MacTLSTest.rsrc
4. CW8 builds with no missing or duplicate symbols
5. Launched MacTLSTest app displays "macTLS Stage A smoke: OK" on G3
6. Same result on G4
```

Smoke-test failure codes (from [os9/ostls_smoketest.h](os9/ostls_smoketest.h))
are kept stable so log lines map directly to the failed gate:

```
100  br_ssl_engine_last_error != BR_ERR_OK or engine CLOSED after reset
101  engine state did not include BR_SSL_SENDREC after reset
102  br_ssl_client_reset() returned 0 (likely BR_ERR_NO_RANDOM)
103  OSTLS_InjectStageAEntropy() returned non-zero
```

If the failure is linker-side (missing/duplicate symbols), check the
file manifest first — re-run `python3 tools/make_bearssl_filelist.py`
and compare against the project's source list. Drift between the two is
the most likely cause.

### A.5 probe gate

Stage A.5 passes when [tools/probes/ppc_mul64/](tools/probes/ppc_mul64/)
reports `ALL PASS` across the **six-cell matrix**:

|              | No optimization | Size optimization | Speed optimization |
|--------------|:---------------:|:-----------------:|:------------------:|
| **G3**       |        ☐        |         ☐         |          ☐         |
| **G4**       |        ☐        |         ☐         |          ☐         |

If any cell fails: do **not** apply the libcss `int64 → double`
workaround (floating point is non-constant-time on PPC and would defeat
BearSSL's CT design). The correct fix is a tiny PPC inline-asm shim
using `mullw` + `mulhwu`, gated by `#ifdef __MWERKS__` inside
i31_moddiv.c. That is one of the few upstream BearSSL patches the
project will accept.

### Stage B/C/D shape (deferred, NOT in scope)

When both gates pass, Stage B begins inside the same Carbon app that
ran the smoke test. The boundary is:

```
Stage B1: OSTLSSocket + memory layout + BearSSL pump over mock transport
Stage B2: Open Transport adapter implementing the transport vtable
Stage C:  HTTP proxy listener on 127.0.0.1:8765 + request parser
Stage D:  upstream HTTPS fetch (BearSSL handshake + GET + response relay)
```

After Stage D, MacTLSTest.mcp gets renamed to MacTLSProxy.mcp (same
project file, just refocused). Stage E is browser compatibility testing
(Classilla, iCab, MacSurf all configured to use 127.0.0.1:8765). Stage
F is trust store + pinning + UI. Stage G is packaging (Startup Items
auto-launch, background-only faceless mode).

Recorded shape (do NOT implement until B1 unblocks):

```c
typedef struct OSTLSTransportVTable {
    OSStatus (*recv)(void *refcon, unsigned char *buf, size_t *len);
    OSStatus (*send)(void *refcon, const unsigned char *buf, size_t *len);
    UInt32   (*flags)(void *refcon);
} OSTLSTransportVTable;
```

This lets the BearSSL pump be exercised deterministically with a
recorded byte stream before OT is in the picture. Same discipline as
the Stage A smoke test: isolate the failure boundary.

### Stage C correction (first real handshake)

When the first real handshake lands (Stage C), it does NOT use
generic X.509 validation. The first real victory condition is:

```
Connect to one controlled host.
Validate one pinned cert/known-key (NOT a root chain).
GET /
Receive plaintext response.
Close cleanly.
```

The local trust-anchor bundle is Stage D, not Stage C. This adjustment
is recorded here so it doesn't drift into the Stage A.5 / B prep.

### Open items the validation gate may surface

These are explicitly NOT being preemptively solved; they're listed so
the user knows what classes of issue to file against, and so the
project state doesn't carry hidden assumptions:

- **Segment limits.** Adding 255 .c files to MacSurf.mcp may trip CW8's
  per-segment code limit. If so: the fix is splitting BearSSL into a
  separate CW8 segment, not source-side surgery. Document and ship.
- **TOC pressure.** PPC PEF has a finite global data table. If the
  combined libcss + libdom + libhubbub + BearSSL build saturates it,
  the per-file `-mno-fp-in-toc` / `-mminimal-toc` flag pattern from
  the deep-research report applies. CW8 has equivalent project-level
  settings.
- **Stack high-water during `br_ssl_client_init_full`.** cryanc
  documented 512 KB stack pressure on a related TLS stack. The Stage A
  smoke test will reveal this empirically: if init succeeds without
  partition adjustment, the existing 16 MB Carbon partition is fine.
  If it crashes mid-init with stack corruption symptoms, bump the
  partition AND/OR move more state to globals before debugging anything
  else.

### Sign-off

Stage A source-side work is frozen as of this commit. No further
source-side changes until validation lands.

Next agent / next session: read this section first. If the Mac
validation has passed, AUDIT.md and README.md will reflect that and
Stage B work can begin. If neither has happened, run the validation —
do not start Stage B speculatively.
