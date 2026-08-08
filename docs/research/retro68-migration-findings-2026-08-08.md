# Retro68 migration: findings and shutdown (2026-08-08)

**Status: experiment closed.** This document is the record of what was tried,
what was found, and why. The `retro68` branch is kept (unpushed beyond this
commit) in case anyone wants to pick it back up; `master` remains the sole
active line and CodeWarrior 8 remains the only shipping toolchain.

## The question

Could [Retro68](https://github.com/autc04/Retro68) (a modern GCC cross-compiler
targeting classic PowerPC Mac OS / Carbon) replace CodeWarrior 8 as MacSurf's
build toolchain? CW8 requires a physical or emulated Mac to run the compiler
itself; Retro68 would let the whole build run on Linux, with a real
`-Wall`/`-Werror` diagnostic surface CW8's `gcc -fsyntax-only` proxy has never
been able to provide (documented gap: it dies inside the Carbon includes and
never reaches a frontend `.c` file's body).

## What was already on the branch before this round

24 commits, all in a single ~4.5 hour session on 2026-08-04. In that window:

- A [CMakeLists.txt](../../CMakeLists.txt) (84 lines) built all ~850 C
  files — libcss, libdom, libhubbub, libparserutils, QuickJS, BearSSL, macTLS,
  NetSurf core, and the macos9 frontend — with `file(GLOB)`/`GLOB_RECURSE`, so
  new source files are picked up automatically (no hand-maintained file list,
  unlike CW8's `.mcp`).
- It **linked**: `MacSurf.pef` was a real 2.5 MB Carbon PEF whose only
  imported shared library was `CarbonLib`, and `MacSurf.bin` was a valid
  MacBinary APPL/MPLS with resource fork, icon family, and a `SIZE` resource.
- It **ran on real OS X 10.3 Carbon CFM hardware** far enough to survive CFM
  load, the CRT, and seven early init calls (`macsurf_tb_calibrate`,
  `macsurf_debug_log_init`, `macsurf_osver_init`, `macsurf_heap_bounds_init`,
  `macsurf_profile_reset`, `MS_LOG`, `macsurf_recon_mem`) — evidenced by two
  `SysBeep` checkpoints firing.
- A real, deep bug had been found and fixed: newlib's `dlmalloc` was winning
  symbol resolution over Retro68's `NewPtr`-based allocator, and `dlmalloc`'s
  `_sbrk_r` (which Retro68 implements as `Debugger()`+`NewPtr`) returns
  non-contiguous blocks that corrupt its arena bookkeeping. Fixed by linking
  Retro68's `libretro/malloc.c` directly and replacing `SetPtrSize` in
  `realloc` with `NewPtr`+`memcpy`+`DisposePtr`.
- Total source divergence from `master`, excluding `build*/` directories: **~900
  lines across 31 files, almost entirely the shim/prefix compat layer**
  (`stdint.h`, `macos9.h`, `sys/time.h`, `time.h`, `stat.h`, `stdbool.h`,
  `macsurf_prefix.h`, plus a new `retro68_malloc.c`). libcss, libdom,
  libhubbub, libparserutils, QuickJS, macTLS and NetSurf core were untouched.

It stalled because every runtime test was on OS X 10.3, the one platform
where Retro68's classic-Mac-OS runtime assumptions are most likely to be
wrong, and diagnosis was SysBeep-bisection with no working log channel.

## What this round did

Assessment: the build-fix side looked genuinely close (per-file divergence
was tiny; the stalling reason was diagnostic method, not scope), so the plan
was to fix the build-quality issues first — since they were cheap to verify —
then get a real crash address on the runtime hang.

### 1. Root-caused (on paper) and fixed the CFM stack

`add_application()`'s `RetroCarbonAPPL.r` template sets the `cfrg`
`appStackSize` field to `kDefaultStackSize`, which Retro68's `Multiverse.r`
defines as **literal 0** — "let CFM pick," and CFM's pick for a PowerPC
application is a few tens of KB. The CodeWarrior project sets a 16 MB stack.
libcss's cascade, libdom's tree walks, NetSurf's layout recursion and
QuickJS's evaluator all recurse deeply; a default CFM stack would be smashed
almost immediately. This matched the observed symptom exactly: CRT survives,
seven shallow inits survive, then nothing further runs.

Verified in the resource fork of the shipped binary before the fix:
`appStackSize = 0`. [MacSurfSize.r](../../browser/netsurf/frontends/macos9/MacSurfSize.r)
was rewritten to emit its own `cfrg` (16 MB stack, matching CW8) alongside
the `SIZE` resource (raised 64/16 MB → 224/128 MB, matching the CW8 project;
16 MB is CLAUDE.md's documented libcss-starvation floor, so the old minimum
sat exactly on it). Confirmed present in the rebuilt binary by parsing its
resource fork directly with Python: `appStackSize=16777216`,
`SIZE preferred=224MB minimum=128MB`.

**This fix did not change the runtime hang.** It was a real, verifiable
defect and is worth keeping if the branch is ever revived, but it was not
*the* answer, and no theory should have been trusted as "the root cause"
without a build-and-test cycle to confirm — which did happen here, and it
came back negative. Recorded here so nobody re-derives and re-ships it as
new information.

### 2. Turned `-w` back on, fixed what it found

The build passed `-w` (all warnings off) — the same trap
[CLAUDE.md](../../CLAUDE.md) documents for the CW8-vs-Linux gap: with
warnings off, a missing declaration silently compiles as `int f()`, so a
Toolbox call that actually returns a pointer or takes a struct is called
through the wrong ABI and crashes at runtime with nothing in the build log.
Switched to `-Wall -Werror=implicit-function-declaration` and fixed
everything that broke, all found in code that also ships on CodeWarrior (i.e.
independent of whether Retro68 goes anywhere):

- **libcss's lexer was being miscompiled.** libcss/libdom/libhubbub/
  libparserutils each ship their own `utils/utils.h` and include it by that
  relative path, but the NetSurf source directory preceded the library
  source directories on the include path, so all four resolved to
  *NetSurf's* `utils/utils.h`. `macsurf_prefix.h:353` already documents
  papering over the *macro* fallout (`N_ELEMENTS`, `SLEN`, `UNUSED`); it
  cannot cover functions, and `libcss/src/lex/lex.c` calls `isDigit()`/
  `isHex()` (return `bool`) and `charToHex()` (returns `uint32_t`) — all
  three were being called with no declaration in scope, i.e. assumed
  `int`-returning. That is a real ABI defect in the CSS lexer, on the code
  path `var()` support depends on. Fixed by giving each library its own
  source root ahead of the shared include list.
- **macTLS gated its Toolbox includes on `__MWERKS__`** (the compiler) rather
  than "is this a Mac target" — so under Retro68, 23 files compiled against
  hand-written stub typedefs instead of real Toolbox headers. Notably,
  `Microseconds()`/`TickCount()`/`GetMouse()` were stubbed to return zero in
  `ostls_entropy.c`, meaning **the TLS entropy pool would have gotten no
  timing entropy at all** under a Retro68 build. Converted the 23 gates to
  `#if defined(__MWERKS__) || defined(__RETRO68__)`.
- The libwapcaplet shim ([libwapcaplet.h](../../browser/netsurf/frontends/macos9/libwapcaplet/libwapcaplet.h))
  claims the real header's include guard, so anything missing from the shim
  can never be supplied by falling through to the real one. Diffed the shim
  against the real API and found five missing declarations
  (`lwc_string_tolower`, `lwc_intern_substring`, `lwc_string_caseless_hash_value`,
  `macsurf_ptr_is_heap`, `lwc__assert_and_expr`) — added all of them rather
  than patching one per build cycle.
- Several core files call `nslog_log()` directly rather than through the
  `NSLOG` macro (`utils/messages.c`, `utils/ns_hashtable.c`,
  `desktop/browser.c`); the prefix suppresses `log.h` entirely (it has GNU
  `__attribute__` and GCC-varargs CW8 rejects), so the direct calls had no
  declaration.
- `layout_internal.h`'s `lh__box_intrinsic_w/h` helpers relied on every
  includer having already pulled in `netsurf/content.h`; `layout_flex.c` and
  `layout_grid.c` had not, so `content_get_width`/`content_get_height` (the
  fixes929 intrinsic-size path) were being called as implicit `int`-returning
  functions.
- Smaller ones: `html/form.c` called `browser_window_set_status` without
  including `desktop/browser_private.h`; `utils/filepath.c` called
  `access(path, R_OK)` with neither declared (fixed via `nsutils/unistd.h`,
  which is what a bare `#include <unistd.h>` resolves to in this tree,
  backed by the real `mac_access()`); `macsurf_qjs.c` had an implicit
  `macos9_content_is_live` and passed `int*` to `JS_ToInt32`, which wants
  `int32_t*` (== `long*` on Mac PPC — CW8 treats `int*`/`long*` as distinct
  types and rejects this outright, so this was latent breakage for the CW8
  side too, just never triggered because `-w` hid it there as well).

### 3. Found two features that were silently dead

- `desktop/treeview.c` (backing `cookie_manager.c`, `global_history.c`,
  `local_history.c` — all three already in the build) was **never in the
  source list**, despite ~20 `treeview_*` symbols being referenced. The link
  only ever succeeded because `-gc-sections` discarded all three managers
  along with every reference to them. Added it to `CMakeLists.txt`.
- `about:testament` and `about:imagecache` handler rows in
  [about.c](../../browser/netsurf/content/fetchers/about/about.c) referenced
  handlers whose source files are deliberately excluded from the Mac build
  (`image_cache.c`, and testament has no Mac-side source at all). Same
  `-gc-sections`-was-hiding-it story. Guarded both rows out under
  `#ifndef __MACOS9__`.

### 4. Found a real toolchain-level type incompatibility

`shims/mac_file_io.c` and `shims/mac_stat.c` call `FSReadFork`/`FSWriteFork`/
`FSSetForkSize`/`FSGetForkSize`, which take/return `SInt64`/`UInt64`. Per
Apple's own `MacTypes.h` comment, those types are "either a struct or a
`long long`, depending on the compiler" — gated on `TYPE_LONGLONG`, which
`ConditionalMacros.h` only sets from Metrowerks's `__option(longlong)`. Under
Retro68's GCC it is unconditionally 0, so these are `struct { SInt32 hi;
UInt32 lo; }`, not scalars — and forcing `-DTYPE_LONGLONG=1` on the command
line does **not** work, because `ConditionalMacros.h` defines the macro
itself and overrides any earlier value (verified directly). Wrote
[shims/mac_int64.h](../../browser/netsurf/frontends/macos9/shims/mac_int64.h),
two macros (`MAC_S64_ZERO`, `MAC_S64_LOW`) that compile to the right thing
under either spelling, rather than using Apple's own `Math64.h` helpers
(which are real out-of-line CarbonLib calls under the struct branch — using
them would trade a compile-time question for a link-time availability
question).

### 5. Found the POSIX shim layer was never being compiled at all

CodeWarrior's `.mcp` builds all five files in `shims/` (`mac_stat.c`,
`mac_file_io.c`, `mac_dirent.c`, `mac_time.c`, `mac_iconv.c`). The Retro68
`CMakeLists.txt` globbed only the top level of `frontends/macos9/` and never
descended into `shims/` — so under Retro68, none of the real Carbon File
Manager implementations were ever linked in. This is what
`tools/apply_retro68_fixes.sh` was compensating for with a generated
`opendir()` stub that unconditionally returned `NULL`. That script also had
a stub that redefined `netsurf_version` — a symbol `desktop/version.c`
already defines — meaning the file it generated **could never have been part
of a link that succeeded**. It had drifted from a working state; the whole
mechanism is marked obsolete in-file rather than deleted, with a note on
where each of its 12 steps ended up (mostly folded into the tree; one item,
the `MacSurfIcon.r` FREF/BNDL-as-data-block conversion, was never migrated
and remains outstanding).

Wrote [macos9_retro68_compat.c](../../browser/netsurf/frontends/macos9/macos9_retro68_compat.c)
as the real bridge: `opendir`/`readdir`/`closedir` over the actual
`mac_opendir`/`mac_readdir`/`mac_closedir` Carbon implementations, plus
honest-failure stubs (documented as such, not silently invented behaviour)
for the handful of newlib-declared-but-undefined POSIX calls Classic Mac OS
has no real analogue for (`chdir`, `mkdir`, symlinks, `sigprocmask`, etc.),
and for zlib/base64 entry points that have no implementation anywhere in the
Linux tree and resolve only against Mac-side files under CodeWarrior.

### 6. Found a genuine ld defect

Once the shim layer compiles, defining the POSIX names retains a
significantly larger amount of reachable code — and **Retro68's own `ld`
segfaults** rather than reporting an unresolved symbol, with no diagnostic
output at all (confirmed: not even a single `-Wl,-t` trace line is emitted
before the crash). Isolated by bisection to be reproducible with a single
object file defining nothing but `closedir()`; the identical link without it
completes normally. This was traced further to a real, separate cause:
Retro68's own newlib and libgcc reference — but never define —
`__gcc_qadd`/`__gcc_qdiv`/`__gcc_qmul`/`__gcc_qsub` (IBM long-double soft
float), `_memalign_r`, and `_jp2uc_l`/`_uc2jp_l`. Those are gaps in Retro68's
prebuilt libraries, not in MacSurf. `-gc-sections` had been silently hiding
this the whole time, by discarding the code paths that would have pulled
those undefined archive members in; it turned out to be **load-bearing
rather than an optimisation** on this toolchain, and stayed on in both build
configurations for that reason (documented at length in `CMakeLists.txt`).

Net effect: the shim layer is written, compiles cleanly, and is behind a
`-DMACSURF_POSIX_SHIMS=OFF` switch so a build can proceed without it. Solving
the `ld` crash is its own separate piece of work.

### 7. Built and shipped

With the shim layer off, the full build completed and produced a
4,293,632-byte `MacSurf.bin`, verified via direct resource-fork parsing to
carry the corrected `cfrg` (`appStackSize=16777216`) and `SIZE`
(`224MB`/`128MB`). Sent to the iMac's `/Projects/MacSurfBuilds/` over the
tunnel (`scp -O` with the Panther-era legacy KEX/HostKey/cipher options —
`diffie-hellman-group1-sha1` / `ssh-rsa` / `aes128-cbc` — none of which
`forclaude/drop-to-imac.sh` carries, since that script is CR-conversion +
mtime-stamping for source files, not binary transfer).

## The result

**Two `SysBeep` checkpoints, then a hang — identical to before any of this
round's fixes.** None of the above changed the runtime symptom.

## Why the experiment was closed here, not pushed further

The maintainer's framing surfaced the real problem with how this round was
being run: *"the only difference between the app running on OS 9 and OS X is
one line that detects what system it is — everything in the stack is proven
to work otherwise."*

That is true of the **CodeWarrior** binary — CLAUDE.md documents it running
identically on OS 9 and OS X 10.0–10.4 from the same CFM binary, both
platforms maintainer-verified. It has never been true of anything Retro68
has produced. The `macsurf_os_is_osx()` check gates a handful of `DIAG` log
lines, not a code path — it is not the axis along which the CW8 and Retro68
binaries differ. What actually differs is the entire toolchain underneath
that check: a different compiler and runtime (GCC/newlib vs
CodeWarrior/MSL), a different malloc (a hand-rolled `NewPtr` wrapper written
and never exercised past `main()` this cycle), a different header set (Apple
Universal Interfaces vs CW8's own, which this round already found real
collisions in), a different POSIX layer (newlib, whose own `ld` cannot even
link the POSIX shim layer without crashing), and a different resource
pipeline (`RetroCarbonAPPL.r`, which needed a stack-size fix just to reach
`main()` at all).

Two beeps means the CRT and seven init calls survive. It does not mean "the
stack" works — under Retro68, almost nothing has ever run past that point,
on **any** OS. The only test that would actually isolate whether this is an
OS-X-specific problem — running the Retro68 build on **OS 9**, the platform
its PPC/Carbon backend was actually built and tested for, via SheepShaver for
fast iteration — was never run. Continuing to debug blind on OS X 10.3
hardware, with no working diagnostic channel (`macsurf_debug_log_init`'s
`FindFolder`/`fopen` path produces no output under Retro68 — a `Note` on the
open list below, not diagnosed), was assessed as further time spent without
new information, and the maintainer ended the experiment at that point.

## Standing bugs found, independent of Retro68's fate

Everything in sections 1–4 above is a real defect in code that also ships
under CodeWarrior. None of it was runtime-verified there this cycle
(CW8 build was not attempted), but each is independently reasoned and
traceable to a specific line:

- **The libcss lexer ABI mismatch** (section 2, first bullet) is the most
  concerning: `isDigit`/`isHex`/`charToHex` compiled as implicit-`int`
  functions is a genuine correctness risk if it also happens under CW8's
  own include-path resolution, wherever a project user path pattern causes
  the same resolution order. Worth an audit independent of Retro68.
- **The `SInt64`/`UInt64` struct-vs-scalar distinction** (section 4) applies
  to any future code that touches `FSReadFork`/`FSWriteFork`/`FSGetForkSize`/
  `FSSetForkSize` or similar Wide-typed Toolbox calls under a non-Metrowerks
  compiler.
- **`macTLS`'s `__MWERKS__`-gated Toolbox includes** (section 2, second
  bullet) is purely a Retro68 concern as written (CW8 already defines
  `__MWERKS__`), but the pattern — gating on compiler identity instead of
  target identity — is worth grep-ing for elsewhere in the tree if any other
  toolchain is ever considered.

## If this is ever revisited

1. Fix `macsurf_debug_log_init()` under Retro68 first — `FindFolder`+`fopen`
   is presumably the failure point, but this was not diagnosed. Without a
   working log channel every subsequent round is SysBeep-bisection again.
2. Test on OS 9 (SheepShaver first, for iteration speed) before doing any
   further OS X debugging. If it also hangs at two beeps on OS 9, the
   problem is toolchain-wide and OS X-specific hardware cycles stop being
   useful. If OS 9 gets further, the OS X path is where the remaining work
   concentrates, same as it did for the CW8 migration originally (see the
   `TARGET_API_MAC_CARBON` history at the top of CLAUDE.md).
3. Solve the `ld` SIGSEGV-on-unresolved-symbol issue (section 6) before
   re-enabling `MACSURF_POSIX_SHIMS` — it currently makes any future
   unresolved-symbol mistake indistinguishable from a genuine linker bug.
4. Re-verify the stack/SIZE fix (section 1) is still needed and still
   correct; it was never disproven, just proven insufficient alone.
