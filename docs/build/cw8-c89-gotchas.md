# CodeWarrior 8 & C89 Gotchas

This is the landmine map. If you're touching MacSurf's C source and the compiler is yelling at you — or worse, the build is clean but the Mac crashes at a garbage address — this page is where to look first. Every entry here cost real debugging time on real hardware. The reader we have in mind is a competent C programmer who has never met CodeWarrior 8, the Macintosh Toolbox, or a PowerPC running cooperative multitasking. We define the platform-specific terms as they come up.

MacSurf compiles with **Metrowerks CodeWarrior Pro 8** running on Mac OS 9 (PowerPC). CodeWarrior 8 compiles in **C89** — the 1989 ANSI C standard, no C99 features — and it has its own quirks on top of that. It defines the macro `__MWERKS__`, which we use throughout the code to gate CodeWarrior-specific workarounds. Source background on the toolchain lives in [Setting Up the Build Environment](Setting-Up-the-Build-Environment) and the exact target settings live in [CodeWarrior Project Settings](CodeWarrior-Project-Settings); this page is about the language and code-generation traps.

The entries are grouped: pure C89/compiler rules first, then the code-generation bug that masquerades as a logic bug, then the Toolbox and Carbon hazards that only bite at runtime. Each one is short: what it is, why it happens, how to step around it.

---

## C89 language restrictions

C89 is stricter than most working programmers remember, and CodeWarrior 8 enforces it. None of these are CodeWarrior bugs — they're the standard. But if you write modern C out of habit, you'll trip them constantly.

**No `inline`.** The keyword doesn't exist in C89. Mark functions `static` and let the compiler decide, or just take the call overhead — on these workloads it rarely matters.

**No `//` line comments.** Use `/* */` block comments everywhere. This one is easy to forget when copying a snippet from a modern codebase.

**No `for (int i = 0; ...)` loop-scope declarations.** You cannot declare a variable inside the `for` statement's parentheses. Declare `i` at the top of the enclosing block and write `for (i = 0; ...)`. This applies to pointer-typed loop variables too (`for (TYPE *p = ...)`), which is the form that's easiest to overlook when auditing a library port.

**All variables at the top of their block.** C89 requires every declaration in a block to come before the first statement. You can open a new `{ }` scope mid-function to introduce a fresh declaration point — that trick shows up a few times in the code where a local is only needed for one operation.

**No C99 designated initializers.** You cannot write `struct foo f = { .name = "x", .id = 3 };`. Initialize positionally, in declaration order: `{ "x", 3 }`. This matters most when porting NetSurf libraries, which lean heavily on designated initializers for their property tables and handler vtables — converting them is a big chunk of any library-port pass. See [Contributing & Expanding](Contributing-and-Expanding) for the port checklist.

**No compound literals, no variable-length arrays, no flexible array members.** These are all C99. If you need a temporary aggregate, declare a named local. If you need a runtime-sized buffer, `malloc` it.

**No forward enum declarations.** `enum foo;` is illegal in C89. More subtly, CodeWarrior 8 also **fails to complete a struct that has a *named* enum declared inside its body**: `struct s { enum e { A, B } type; ... };` leaves both `struct s` incomplete and `e` undefined. An *anonymous* enum inside a struct (`enum { A, B } type;`) is fine. The fix is to hoist the named enum to file scope before the struct that uses it. We hit this with `html.h`'s `enum html_script_type`.

### Variadic macros: `__VA_ARGS__` works, fixed-param "varargs" macros don't

This is the one that surprises people, because it's backwards from intuition. `__VA_ARGS__` is technically C99, but CodeWarrior 8 accepts it as a pre-C99 extension. So this is legal:

```c
#define NSLOG(cat, level, ...) do { } while (0)
```

What does *not* work is the older trick of defining a fixed-parameter macro and calling it with extra arguments, e.g. `#define NSLOG(cat, level, fmt)` invoked as `NSLOG(fetch, DEBUG, "x", y)` — CodeWarrior rejects that with `')' expected`. And the seductive-looking alternative of forwarding to a real variadic *function* fails for a different reason: the macro arguments get evaluated as expressions. `NSLOG(fetch, DEBUG, ...)` expanding to `noop_(fetch, DEBUG, ...)` requires `fetch` to be an in-scope identifier — but `fetch`, `llcache`, `layout`, `flex`, and `schedule` are category *tokens* in NetSurf's logging, not variables, so you get `'fetch' undeclared` wherever the category isn't a real local.

So the NSLOG pattern is: **`#define NSLOG(cat, level, ...) do { } while (0)`** — the category and level are consumed as unevaluated macro parameters and never touched. (Retro68's PowerPC GCC in `-std=c89` mode accepts this cleanly too, which matters for the Linux pre-flight; see [Cross-Developing from Linux](Cross-Developing-from-Linux).)

**Corollary — do not `#define` the category tokens.** It's tempting to make NSLOG work by defining `fetch`, `llcache`, etc. to `0`. Don't: those words are used as variable, parameter, and struct-member names all over the codebase. `#define llcache 0` turns `static struct llcache_s *llcache = NULL;` into `static struct llcache_s *0 = NULL;` — a syntax error. The `__VA_ARGS__` form makes these defines unnecessary, which is the whole point.

### C89 forbids casting to a union type

`(some_union_type)expr` is a GNU extension, not standard C. CodeWarrior rejects it: *"illegal explicit conversion from 'int' to 'union'."* libcss's generated code does this when zeroing properties — `set_width(style, 0, (css_fixed_or_calc)0, CSS_UNIT_PX)`, where `css_fixed_or_calc` is a `typedef union`. The C89-clean rewrite is a one-shot local in a fresh scope:

```c
{
    css_fixed_or_calc foc_zero_;
    foc_zero_.value = 0;
    set_width(style, 0, foc_zero_, CSS_UNIT_PX);
}
```

When you're scanning a library port for this, note that it's a quiet grep target: `(typename)0` looks innocent unless you happen to know `typename` resolves to a union typedef. The same warning applies to union *initializers* — `{ .field = v }` syntax for a union is illegal in C89; use the positional `{ v }`, which sets the first member only.

---

## The `long long` / `int64_t` multiply-by-constant miscompile

This is the worst kind of bug: the compiler accepts the code, generates wrong instructions, and the wrongness only shows up as subtly broken layout three subsystems away. It is not a C89 rule — it's a genuine code-generation defect in CodeWarrior 8's PowerPC backend.

The defect: `(long long)a * small_const` writes `a >> log2(const)` into the high word of the 64-bit result instead of the correct `(a * const) >> 32`. Confirmed on real hardware — `(long long)131072 * 1024LL` produced a full product of 549,890,031,616 instead of the correct 134,217,728. This quietly broke every fixed-point multiply and divide in libcss for weeks and looked, for all the world, like a CSS layout bug.

The reason it's so dangerous in MacSurf specifically: libcss does fixed-point arithmetic (CSS lengths are stored as `css_fixed`, a 16.16 fixed-point integer), and the natural way to multiply two of those without overflow is to widen to 64 bits, multiply, and shift back down. That widen-multiply-shift path is exactly what miscompiles.

**The workaround: route 64-bit fixed-point math through `double` under `#ifdef __MWERKS__`.** PowerPC has a hardware FPU, and IEEE 754 doubles have a 52-bit mantissa, which covers every 32-bit fixed-point intermediate without loss. The reference pattern lives in `browser/netsurf/include/libcss/fpmath.h`. Pure 32-bit integer multiplies and divides are fine — the bug is *specifically* the 64-bit shift-multiply path. So any code reaching for `int64_t` or `long long` fixed-point math on this compiler is suspect and needs either the `double` treatment or a proof that the operands stay small enough that the miscompile happens to land on the right answer (which is how `INTTOFIX(128)` slipped by — `128 >> 10` is `0`, which happens to be the correct high word).

---

## Header, shim, and include-guard traps

MacSurf shims out POSIX and stubs unported dependencies with replacement headers under `frontends/macos9/`. CodeWarrior's **access paths** (its ordered search-path list, see [CodeWarrior Project Settings](CodeWarrior-Project-Settings)) are configured so those directories come *before* the real NetSurf headers, which lets a shim shadow the real thing. That shadowing is a feature when you want it and a foot-gun when you don't. Access paths are searched in order, and a file found on an earlier path wins.[^accesspaths]

**A shim that sets the real guard without defining the real contents breaks every translation unit that includes it.** If a shim `html/html.h` sets the *real* include guard `NETSURF_HTML_HTML_H` but doesn't define `struct html_script`, then when the real header is later reached its guard is already set, so it's skipped — and you get `incomplete type 'struct html_script'` everywhere. The fix is to make the shim a *forwarder* with its own distinct guard that `#include`s the real header by full path:

```c
#ifndef MACSURF_SHIM_HTML_H
#define MACSURF_SHIM_HTML_H
#include "content/handlers/html/html.h"
#endif
```

**The inverse trap: a stub that uses a *different* guard than the header it shadows gets included twice.** If a stub `libwapcaplet/libwapcaplet.h` uses guard `LIBWAPCAPLET_LIBWAPCAPLET_H` but the real header uses `libwapcaplet_h_`, both files can be processed in one translation unit (each finds its own guard unset), producing "illegal name overloading" for every redefined type. The rule that resolves both cases: **make a stub's guard identical to the real header's guard.**

**CodeWarrior can't resolve a path-prefixed relative include from inside a header found via an access path.** When a header that was itself located through an access path does `#include "subdir/foo.h"`, the local-directory context is gone and the lookup fails. A bare basename — `#include "foo.h"` for a sibling file — generally works; a prefixed path like `#include "utils/list.h"` does not. So `autogenerated_computed.h` (in `src/select/`) must include its sibling as `"calc.h"`, not `"select/calc.h"` — the latter resolves to the non-existent `src/select/select/calc.h`. Keep header-to-header includes either bare-basename or fully self-contained.

**`<time.h>` interception.** With the access paths configured the way they are, if a `sys/` directory containing a `time.h` is on the user paths, *every* project-wide `#include <time.h>` finds our `sys/time.h` first — including the one inside `sys/time.h` itself, which loops. The fix was to break the circular include and inline the needed `struct tm` / `time_t` / `localtime` declarations directly.

**`struct dirent` double-definition from the prefix file.** `macsurf_prefix.h` injects `mac_dirent.h` for every CodeWarrior translation unit (it defines `struct dirent` under guard `MAC_DIRENT_H`). If a `.c` file then includes `<dirent.h>` → `shims/dirent.h` with a *different* guard, `struct dirent` gets redefined. Same lesson as above, applied to the prefix: any shim for a type family the prefix injects globally must claim the *injected* header's guard, not invent a new one.

**Suppressing a typedef you still need.** `macsurf_prefix.h` defines `NETSURF_LOG_H` to block `log.h` (which uses GCC-style `##args` and `__attribute__` annotations CodeWarrior can't parse). But `log.c` includes `log.h` to get `typedef bool (nslog_ensure_t)(FILE *fptr)` for the signature of `nslog_init` — so suppressing the header left that typedef undeclared and `nslog_init` failed to parse. When you suppress a header by predefining its guard, audit it for typedefs that something downstream still needs, and reproduce them. (We moved `nslog_ensure_t` into the prefix file itself.)

**Suppressing Carbon.h's deeper sub-headers.** This is the same technique used against MacSurf rather than for it. `Carbon.h` chains into headers that CodeWarrior 8's SDK can't fully parse (e.g. `InternetConfig.h` uses `AliasRecord` by value before `Aliases.h` is processed; `ATSLayoutTypes.h` pulls in C11 anonymous members). You can't fix this by pre-including the dependencies — they chain back into Carbon.h and re-trigger the problem. The clean move is to **define the unwanted sub-header's own include guard before `#include <Carbon.h>`**, so Carbon.h's own guard check skips it. Universal Interfaces use the convention `#ifndef __HEADERNAME__`, so `#define __INTERNETCONFIG__` before Carbon.h skips InternetConfig.h entirely. This is only safe when MacSurf doesn't use that API — which, for Internet Config, ATS layout, Keychain, and the rest of the suppressed set, it doesn't. The active defines live in `frontends/macos9/macos9.h`.

**`int *` and `int32_t *` are incompatible pointers here.** On Mac PowerPC, `int32_t` is `long`, not `int`. CodeWarrior treats `int *` and `long *` as distinct types and refuses to convert between them: *"illegal implicit conversion from 'long *' to 'int *'."* When a shared helper declared `int *ctx` but the autogenerated callers passed `int32_t *ctx`, it was a hard error. Keep the signatures consistent — we standardized on `int32_t *`.

**Public headers that use `nserror` must include `utils/errors.h` first.** `nserror` is a typedef'd enum. If a header (`netsurf/layout.h`, `netsurf/window.h`) uses it in a struct member before `errors.h` is processed, CodeWarrior treats `nserror` as implicit `int`, then conflicts when the real `typedef enum {...} nserror` arrives. Add `#include "utils/errors.h"` at the top of any public header that names `nserror`.

---

## The struct-padding intern crash in `css_computed_style_i`

This one is worth understanding in full, because it's a class of bug — alignment padding turning into a comparison mismatch — that you can re-introduce without noticing.

libcss interns computed styles to deduplicate them, and the dedup test at `src/select/arena.c` is a raw `memcmp(&a->i, &b->i, sizeof(struct css_computed_style_i))` over the whole inner struct. That means **every byte of `css_computed_style_i` must be deterministic across all cascade paths**, or two logically-equal styles compare unequal, the intern table fills with duplicates, and the duplicates get freed while still referenced — a use-after-free in `css_computed_style_destroy` at end-of-convert. The crash signature is distinctive: the log ends right after `content broadcast READY` with no `reformat:` line.

Two distinct ways to break it, both real:

1. **Sub-`int32` scalar fields create non-deterministic padding.** Adding a `uint8_t` or `uint16_t` between two `int32_t` fields makes the compiler insert alignment padding. Cascade code writes the field byte; nothing writes the padding. `calloc` zeroes it at allocation, but a compose-by-copy-then-modify path can carry *source* padding into the destination, so different paths leave different garbage in the gap, and `memcmp` flags equal styles as different. **Always use `int32_t` for scalar fields in `_i` so they self-align.**

2. **Variable-size or per-cascade-divergent data doesn't belong in `_i` at all.** Pointer fields, content arrays, font-family lists, and the like live in the *outer* `css_computed_style` struct and have dedicated `arena__compare_*` functions for *logical* comparison. An inline array in `_i` (we tried `int32_t macsurf_grid_tracks[8]` for grid) gets bit-compared, and any cascade path that doesn't write byte-identical values for logically-equal styles poisons the intern table. The fix pattern for that kind of data: add a pointer field to the outer struct, allocate it during cascade, write a comparison function in `arena.c`, and register it in `css__arena_style_is_equal`. Bit-packed flags in `bits[N]` are fine because every cascade path writes the bit slot deterministically.

A related sequencing rule from the same area: **new fields in `css_computed_style_i` go at the *end* of the struct, never in the middle.** Mid-insertion shifts every later field's byte offset, and CodeWarrior's header-only edits don't always trigger a recompile of every dependent `.c` — so some libcss object files keep the old offsets and you get a silent mid-reformat crash. (When a header change isn't being picked up, touch the dependent `.c` rather than nuking all the object code; that's the right lever, and DIRECTIVE #1 says we never blame "stale files" — we find the offset mismatch and fix it.)

---

## QuickDraw: CopyBits and CopyMask are *colorizing* copies

Now we leave the compiler and enter the Toolbox. QuickDraw is the classic Mac 2D graphics library, and it has a behavior that will silently tint your images: **`CopyBits` and `CopyMask` blend the transferred pixels toward the port's current foreground/background colors.** Even a 32-bit-to-32-bit `srcCopy` is affected. Source pixels are mapped between the port's `RGBForeColor` and `RGBBackColor`, so the *only* identity transform is `fg = black`, `bg = white`.

MacSurf draws blue link text by setting `RGBForeColor(blue)`. If the image blit runs afterward without resetting, every image gets tinted toward whatever color the last text or rectangle draw left behind — dark images go blue, bright ones wash out. Because it depends on paint order and page content, it *looks* intermittent ("fine on first paint, blue after scroll"), which sent the diagnosis chasing the decoder and a phantom channel-swap before a dest-readback probe caught a black source pixel landing in the composite as blue.

**The fix is one line before every blit: set `RGBForeColor(black)` and `RGBBackColor(white)` immediately before any `CopyBits` / `CopyMask`** (in `plotters.c`, `macos9_plot_bitmap`). The general rule for the whole frontend: **any new QuickDraw blit must reset fg=black/bg=white first** unless it genuinely wants the colorizing behavior. Assume the port's foreground is whatever the last draw left it. When an image renders the wrong color, check the blit's fg/bg *before* suspecting the decoder.

---

## UPP / MixedMode: pass routine descriptors, not raw function pointers

To understand this one you need a little Mac history. Classic Mac OS ran 68000 code and PowerPC code side by side, and the **Mixed Mode Manager** handled the switch. A **Universal Procedure Pointer (UPP)**, also called a *routine descriptor*, is a small structure describing a callback's calling convention and address — it begins with a 68000 instruction so it can be invoked from either world.[^upp] You create one with `NewXxxUPP(proc)` and the Toolbox calls *through* it.

The trap: on **CarbonLib on OS 9**, Toolbox calls like `TrackControl` and `InstallEventHandler` still dispatch their callbacks through MixedMode, so the UPP argument must be a real routine descriptor — *not* a bare PowerPC function pointer. There's a tempting micro-optimization that says "Carbon UPPs are just native function pointers, so I'll override `NewControlActionUPP(proc)` to `((ControlActionUPP)(proc))`." That's true for Mach-O Carbon.framework on Mac OS X. It is **false** for CarbonLib on OS 9. Under CarbonLib, MixedMode reads descriptor fields out of your function's entry bytes, resolves a garbage address (usually 0), and executes `bl 0`. The crash signature is a PC down in very low memory (e.g. `0x00000008`) with `LR` matching — because `bl` at address 0 sets `LR = 4` and the CPU walks forward through low-memory globals until the first illegal opcode. The application name in the crash is often `CodeWarrio...` because the CW runtime captures the low-memory trap.

The right approach: don't cast function pointers to UPPs. Either let CodeWarrior's Universal Interfaces expand `NewXxxUPP` normally (CarbonLib *does* export `NewRoutineDescriptor` and friends), or avoid the action-UPP path entirely. For the scroll bar we call `TrackControl(ctrl, pt, NULL)` and respond on return using the part code from `FindControl` plus `GetControlValue()`. The override was removed in fixes147 after it crashed scroll-bar clicks every time.

A neighboring hardware-specific hazard worth knowing while you're in this code: the Appearance **live-tracking** scroll bar CDEF (`kControlScrollBarLiveProc = 386`) crashes on real G3/G4 hardware (into an Appearance Manager internal) but *not* in SheepShaver. The non-live CDEF (`kControlScrollBarProc = 384`) is crash-free because it doesn't do per-frame app callbacks during the thumb drag. The lesson — emulator-green is not hardware-green — is general, and [Diagnostics & Debugging](Diagnostics-and-Debugging) goes deeper on it.

---

## Carbon events have per-environment availability

Carbon is one API, but it has two runtimes: the modern Mac OS X Carbon.framework and the OS 9 **CarbonLib** shared library. Not every Carbon Event exists in both. Apple's own `CarbonEvents.h` annotates each event with where it's available — either `Mac OS X: 10.x+` *and* `CarbonLib: 1.x+`, or `CarbonLib: not available`.

The concrete bite was the mouse wheel. `kEventMouseWheelMoved` was added in Mac OS X 10.0 and **never back-ported to CarbonLib** — `CarbonEvents.h` marks it `CarbonLib: not available`. You can *register* a handler for it without an error, but it never dispatches, and worse, CarbonLib's dispatcher destabilizes when something downstream tries to deliver an event whose class it doesn't recognize. The result was an illegal-instruction crash at a heap-looking address that *looked* like our handler's fault — the handler can be flawless (correct pascal calling convention, proper UPP, initialized `EventTypeSpec`, every classic mistake absent) and still take the blame for CarbonLib walking uninitialized dispatch state.

**The rule: check `CarbonEvents.h` before registering any handler.** If it says `CarbonLib: not available`, the platform cannot deliver that event — the fix is to *not install the handler*, not to debug it. The wheel handler is retained as a visible no-op for ABI stability. (Practical fallout: scrolling on OS 9 goes through the scroll bar, keyboard arrows, Page Up/Down, and Home/End; users are advised to set USB Overdrive's wheel action to "Do Nothing" while MacSurf is frontmost. Full case study in [Diagnostics & Debugging](Diagnostics-and-Debugging).)

---

## CR line endings on every source file

Last one, and it's mechanical but easy to forget. Files destined for the Mac side of the build — `.c`, `.h`, `.r` — need **classic-Mac CR (`\r`) line endings**, not Unix LF. CodeWarrior and the Mac tools expect CR. If you edit on Linux (the normal workflow; see [Cross-Developing from Linux](Cross-Developing-from-Linux)), convert before packaging:

```bash
sed 's/$/\r/' < unix_file.c | tr -d '\n' > mac_file.c
```

This applies only to *source moving to the Mac*. The wiki pages themselves stay LF — they're not part of the build.

---

## A quick mental model

Most of these traps fall into three buckets, and knowing which bucket you're in tells you where to look:

- **The compiler rejected my code.** It's almost always a C89 rule — a `//` comment, a designated initializer, a loop-scope declaration, a union cast, or a header-ordering / include-guard problem. The error message usually points right at it.
- **The build is clean but the Mac crashes.** Suspect code generation (the `long long` miscompile), the intern-comparison padding bug, or a Toolbox misuse (a function pointer where a UPP was expected, an event class CarbonLib doesn't know). Crash *addresses* are diagnostic here — low-memory PC means MixedMode dispatch failure, a garbage heap address often means a poisoned table.
- **It renders, but wrong.** Think QuickDraw color state (the colorizing copy), then layout math (fixed-point, which loops back to the `long long` miscompile).

When you find a *new* member of any of these families, add it here — this page only stays useful if it keeps absorbing the next round's hard-won lesson. And per the project's first directive: when symptoms smell like a "stale file," they aren't. The answer is a real root cause in the code.

---

## Sources

Most entries above are distilled from MacSurf's own `CLAUDE.md` "Known Gotchas" list — hard-won on real hardware, not from a manual. The external/historical claims are cited below.

[^accesspaths]: CodeWarrior access paths are an ordered System/User search list, searched in order, non-recursive by default — a file found on an earlier path shadows a later one. Documented behavior in the CodeWarrior IDE; see the toolchain research note and the NXP-hosted CodeWarrior IDE docs: <https://docs.nxp.com/bundle/GUID-9FAC1C79-3809-474F-B8DF-82BEB5B88419/page/GUID-E8DEA010-B02F-4048-84C1-49B6094FFFE9.html>

[^upp]: Universal Procedure Pointers / routine descriptors and the Mixed Mode Manager: <https://orangejuiceliberationfront.com/universal-procedure-pointers/> and <https://mjtsai.com/blog/2013/03/31/universal-procedure-pointers/>. Carbon as one API across Classic Mac OS and Mac OS X (and thus CarbonLib vs. Carbon.framework): <https://en.wikipedia.org/wiki/Carbon_(API)>.
