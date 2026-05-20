# Cross-Developing MacSurf from Linux

Everything I've learned about shipping Mac OS 9 code from a Linux desktop, plus the round-trip workflow that produced fixes 100+ through 151.

The platform is unusual enough that a lot of this isn't written down anywhere else. This doc is the survival kit.

---

## 1. Platform constraints (what runs on the Mac)

### Compiler

CodeWarrior 8 Pro (with 8.3 update) on Mac OS 9. **Strict C89.** Hard restrictions worth tattooing on your forearm:

- No `inline` keyword (define it away: `-Dinline=`)
- No `//` line comments
- No variadic macros — but `__VA_ARGS__` is supported as a pre-C99 extension and is the safe path for NSLOG-style no-op macros
- No forward enum declarations (`enum foo;` is illegal)
- No C99 designated initializers (`{ .field = value }`)
- No for-scope variable declarations (`for (int i = ...)`)
- No compound literals
- No `restrict`, `long long` is technically supported but **miscompiled** (see §3)
- No GNU union casts (`(union_type)0`) — use a compound statement with a local variable
- All variables at the top of their enclosing block

C89 also rejects named enums inside struct bodies — CW8 silently fails to complete the struct, leaving it incomplete and every member undefined. Hoist named enums above any struct that uses them.

### Bit-pack scalar fields, but watch padding

`css_computed_style_i` (libcss inner struct) gets `memcmp`'d by arena interning to deduplicate styles. **Any new field's byte representation must be deterministic across every cascade path.** This means:

- A `uint8_t` between two `int32_t` creates 3 padding bytes the compiler chooses; cascade code writes the byte but nobody writes the padding. Different code paths can leave the padding with different values → memcmp false-negatives → intern table fills with duplicates → use-after-free during style destroy. **Use `int32_t` for scalar fields in `_i` so they self-align.** This is fixes117 (inline track array crash) and fixes151b (uint8 field crash) — the same trap twice.
- Variable-size data (track arrays, content arrays, font-family lists) goes in the OUTER `css_computed_style`, not in `_i`. Add a dedicated `arena__compare_*` function for it. fixes118 documented this pattern; fixes150 reused it for row tracks.

### Carbon CFM app requirements

- **`'carb'` resource is mandatory.** Without it, CarbonLib never loads, and any `*InContext` Open Transport call crashes inside OTClientLib at a fixed address. Ship `MacSurf.rsrc` (a pre-built binary resource fork; CW8 links `.rsrc` into the output with no Rez step) and list it in the project.
- Skip `InitGraf` / `InitFonts` / `InitWindows` / `InitMenus` / `TEInit` / `InitDialogs` under Carbon. Keep `InitCursor()` and `FlushEvents(everyEvent, 0)`.
- Call `RegisterAppearanceClient()` after `InitCursor()`, gated by a Gestalt check.
- No `kWindowStandardHandlerAttribute` on `CreateNewWindow` — it intercepts updateEvts and leaves windows blank.

### Open Transport rules

- Use plain `OTOpenEndpoint` / `InitOpenTransport`, **not** the `*InContext` variants. Every CarbonLib crash we've debugged has traced to InContext routing through OTClientLib in an uninitialized state.
- `OTUseSyncIdleEvents(ep, true)` plus a notifier that calls `YieldToAnyThread()` on `kOTSyncIdleEvent` — this is how synchronous OT calls cooperate with the rest of the app.
- Carbon CFM cannot do passive (listening) OT binds. The OS rejects it categorically; we verified across 14 rounds in macSSL before pivoting. Don't try to write a local server.

### No preemptive threads

OS 9 is cooperative multitasking. Run a `WaitNextEvent` UI loop. Long-running work yields via `YieldToAnyThread()` or by returning to WNE.

### Memory partition

Carbon partition must be **at least 16 MB preferred / 8 MB minimum**. libcss allocates via raw `malloc`/`calloc` with no NetSurf wrapper — a 4 MB partition (the CW8 default) starves the CSS cascade on real pages. Set via `MWProject_PPC_size` / `MWProject_PPC_minsize` in the `.mcp` XML.

### CW8 PPC long-long codegen is broken

`(long long)a * small_const` writes `a >> log2(const)` into the high word instead of `(a * const) >> 32`. Confirmed on real hardware via probe G (fixes113): `(long long)131072 * 1024LL` produced `hi=128, lo=134217728` instead of the correct full product 549,890,031,616.

**Mitigation:** for any 64-bit fixed-point math, route through `double` under `#ifdef __MWERKS__`. PPC has a hardware FPU and IEEE 754's 52-bit mantissa covers every int32 fixed-point intermediate. See `browser/netsurf/include/libcss/fpmath.h` for the reference pattern.

Pure int32 multiplies and divides are fine. The miscompile is specifically the 64-bit shift-multiply path.

### Carbon event class availability

Mac OS X Carbon events that were never back-ported to CarbonLib (e.g. `kEventMouseWheelMoved`) will register without error but never dispatch, and CarbonLib's dispatcher destabilizes when something downstream tries to deliver an event whose class it doesn't know. Symptoms: illegal-instruction crash at a fixed-looking address inside CarbonLib.

Check Apple's `CarbonEvents.h` annotations before registering any handler. If `CarbonLib: not available`, the platform cannot deliver that event — don't install a handler, don't debug your handler, accept the platform constraint.

### Live-tracking scroll bar CDEF is hardware-specific

`kControlScrollBarLiveProc = 386` crashes on real G3/G4 hardware (not reproducible in SheepShaver). Use `kControlScrollBarProc = 384` (non-live) instead. Trade-off: thumb drag doesn't scroll until mouseup. fixes159 documented this.

### UPP macro override on CarbonLib is unsafe

CarbonLib's `TrackControl` / `InstallEventHandler` expect MixedMode `RoutineDescriptor` UPPs, not raw PPC function pointers. The "Carbon UPPs are just native function pointers" shortcut only works on Mac OS X's Mach-O Carbon.framework, not CarbonLib on OS 9. Symptom: PC in very low memory (e.g. `0x00000008`) with CurApName showing `CodeWarrio...`. Either let CW8's Universal Interfaces expand the macro normally, or use the `TrackControl(ctrl, pt, NULL)` no-callback path and read state via `GetControlValue()` on return.

---

## 2. CW8 header and access path quirks

### Suppress Carbon.h sub-headers by pre-defining their guards

Carbon.h transitively pulls in InternetConfig.h, MacWindows.h, KeychainCore.h, ATSLayoutTypes.h, SFNTLayoutTypes.h — each of which has at least one C89 incompatibility or missing forward declaration. The clean workaround is to `#define __INTERNETCONFIG__` (or whatever Universal Interfaces guard) before `#include <Carbon.h>`. Carbon.h's own guard check sees the symbol defined and skips the include entirely.

Per `macos9.h`:

```c
#define __INTERNETCONFIG__
#define __KEYCHAINCORE__
#define __ATSLayoutTypes__
#define __ALIASES__
struct AliasRecord;
typedef struct AliasRecord *AliasPtr;
typedef AliasPtr *AliasHandle;
#include <Carbon.h>
```

The AliasHandle forward decl is needed because MacWindows.h uses `AliasHandle` in parameter types even with Aliases.h suppressed.

### Shim headers must use the same include guard as the real header

If a shim header sets its own guard (e.g. `LIBWAPCAPLET_LIBWAPCAPLET_H`) but the real library header uses a different guard (e.g. `libwapcaplet_h_`), CW8's access path will find both in the same TU. The stub processes first, sets its own guard; later in the same TU the real file is found via a different path, its guard is unset, it processes too, and you get "illegal name overloading" on every type both files define.

**Always make the stub's guard identical to the real header's guard.** Applied in fixes264 for `libwapcaplet/libwapcaplet.h` and `css/utils.h`.

### CW8 can't resolve relative includes from headers found via access paths

When a header found via an access path does `#include "subdir/foo.h"`, CW8 can't find foo.h because the local-directory context is lost. Simple basenames (`#include "foo.h"`) work if the target is in the same directory. Path prefixes (`#include "utils/list.h"`) fail.

**Workarounds**: make headers self-contained (inline the content), or use access paths that put the target file in the same search directory.

### macsurf_prefix.h's globals

The prefix file gets injected before every TU. It must:

1. Include `<MacTypes.h>` first (otherwise `bool`/`true`/`false` conflict with later includes)
2. Define `__MACOS9__ 1`
3. Define `NO_IPV6 1`
4. Define `TARGET_API_MAC_CARBON 1`
5. Define `MACSURF_DEBUG 1` (gated on `#ifndef MACSURF_RELEASE`) — this is what enables the file-backed debug log; without it the entire log channel compiles to empty stubs (fixes305a regression)
6. Stub `NSLOG` via `#define NSLOG(cat, level, ...) do{}while(0)`. CW8 supports `__VA_ARGS__` as a pre-C99 extension. **Do not** use a varargs C function — it forces the category token (`fetch`, `llcache`, `layout`, etc.) to be evaluated as an expression and fails with `'fetch' undeclared` in TUs where those names are not in scope.

### Two-source dispatch.c gotcha

`libcss/src/select/dispatch.c` and `libcss/src/select/s_dispatch.c` both define `prop_dispatch[]`. The Mac builds `s_dispatch.c` (basename match per commit `a2f5656d`'s flat-folder rename); the Linux side has both as a historical accident.

- **Only one belongs in MacSurf.mcp.** Both = duplicate-symbol link error.
- **Keep both byte-identical when adding properties.** Otherwise CSS_N_PROPERTIES grows but only one table grows with it → out-of-bounds dispatch → unmapped-memory exception (fixes116b crash signature `0x68F168F0`).
- Ship both files in the fix tar even though only one is built, so they don't silently drift.

### `int *` and `int32_t *` are incompatible on CW8 PPC

On PPC, `int32_t = long` (not `int`). CW8 is strict about pointer types: passing `int *` where `int32_t *` is expected fails with "illegal implicit conversion from 'long *' to 'int *'". Common trap: autogenerated libcss parse files have `int32_t *ctx`, but a shared utility declared `int *ctx`. Make all signatures consistent.

### Forward enum declarations are illegal

Any C99 header that uses `enum foo;` to forward-declare an enum will fail on CW8 with "undefined identifier 'foo'". Replace with the actual `#include` of the header that provides the full enum.

---

## 3. Workflow: Linux → tar → scp → Mac

### Repo layout

```
~/Webs/macsurf/                   primary working tree (Linux git)
~/Documents/macfiles/             outbound tar staging
   fixes149.tar, fixes150.tar...  one tar per shipped round
   Old Zips/                      archived once a round is closed
```

### Per-round process

1. **Edit on Linux** in the macsurf working tree.
2. **Pre-flight on Linux** with Retro68 PPC GCC for C89/CW8 compatibility:

   ```
   /home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc \
     -std=c89 -pedantic-errors -Wall -Wno-long-long -Wno-pedantic \
     -fsyntax-only -Dinline= -D__MACOS9__ -DCSS_INTERNAL \
     -I[various include paths] \
     path/to/changed_file.c
   ```

   Pre-existing errors from the libcss/lwc shim mismatch are OK and pass on CW8 — what matters is that no NEW errors appear at the edit sites.

3. **Bundle as a tar with the full path tree preserved**:

   ```
   tar -cf ~/Documents/macfiles/fixesNN.tar \
     browser/libcss/src/parse/properties/p_foo.c \
     browser/libcss/src/select/properties/s_foo.c \
     [other changed files...]
   ```

   The tar contains *only the files changed in this round* — never a full source dump (that would clobber the user's local edits to MacSurf.mcp / Access Paths). After the initial bootstrap round, every tar is delta-only.

4. **Ship via scp**:

   ```
   scp -P 2222 -i ~/.ssh/macsurf_push \
     ~/Documents/macfiles/fixesNN.tar \
     patrick@localhost:Documents/macfiles/
   ```

   The user's Mac is reachable via an SSH bridge on localhost port 2222.

5. **Mention any new `.c` files for MacSurf.mcp**. Do not edit the `.mcp` file from Linux — the user maintains the project file list on the Mac via the CW8 IDE, and a Linux-edited `.mcp` would clobber their local state.

6. **Commit on Linux** with a substantive commit message that captures the architectural intent. Commits are local working memory and become CLAUDE.md material when the round closes.

7. **User unpacks on the Mac**, adds any new `.c` files to MacSurf.mcp via CW8, rebuilds with "Remove Object Code" first, runs the binary.

8. **User reports back** via screenshots and/or the file-backed log (see §4).

9. **Update CLAUDE.md** to record what shipped (the "Last shipped fix" entry rolls forward, with predecessors demoted below).

### What goes in the tar

Source files only — never `.mcp`, never `Access Paths.xml`, never docs. The tar's footprint stays small (tens of KB up to ~300 KB for big rounds with autogenerated headers).

### Numbering

The user owns the fix number. If they say a round is `fixes149`, that's what it is — don't argue with the numbering, just adopt it. Letter suffixes (`fixes148b`, `fixes148b3`) are used within a single feature's iteration when the first shipped attempt needed follow-up.

### Don't blame stale files

The user's build is what they say it is. If something looks like staleness, the answer is to find a real root cause in code — a missing include, a wrong field path, an autogen mismatch — not to lecture about CW8 caches or zip-extraction failures. If you genuinely think a previously-shipped file may need to be reshipped because symptoms continued, just include it in the next zip with no narration about why. **DIRECTIVE #1 in CLAUDE.md.**

---

## 4. Diagnostics: file-backed log + hardware-only verification

### The log channel

`browser/netsurf/frontends/macos9/macsurf_debug_log.c` writes one CR-terminated line per `MS_LOG(msg)` or `macsurf_debug_log_writef(fmt, ...)` call to `MacSurf Debug.log` on the Desktop, flushing after every write (`FlushVol` + `SetFPos`). This is the **primary post-crash backtrace channel** since we can't attach MacsBug remotely.

- Located via `FindFolder(kOnSystemDisk, kDesktopFolderType, ...)`. If FindFolder fails the channel is silently inert.
- Format specifiers supported: `%d`, `%ld`, `%p`, `%s`, `%%` (and `%lx` is NOT supported — use `%ld` and decode hex by eye if you need it).
- Output capped at 255 bytes per call.
- Gated on `MACSURF_DEBUG` in macsurf_prefix.h. The `#ifdef MACSURF_DEBUG` test happens inside the implementation; if the define is missing, every call site compiles to nothing (fixes149/150/151 silent-instrumentation regression, then fixes305a same vector).

### Subsystem-init audit checklist

After fixes149's three rounds of silently-failing instrumentation, we adopted this checklist:

1. Init function wired in `main.c`? `grep -c subsystem_init main.c` must be > 0.
2. Init function body **actually reachable**? `#ifdef MACSURF_DEBUG` and similar feature-macro gates can compile the body to an empty stub. `grep -rn "define MACSURF_DEBUG"` must turn up at least one site.
3. Smoke-test confirming the subsystem produces its externally-visible artefact at startup (log file appears, title bar updates, menu item present).

Step 2 is the new one — it catches the fixes305a vector where the init call is present but the macro is missing.

### Verifying via hardware probes on advanced.html

Most CSS feature work ships with **probe cards** added to `mactrove.com/advanced.html` (lives on the Drupal proxy, not on the Mac, so refreshing the URL is enough — no Mac-side ship). Each fix sprint adds a section with N labelled probe cards (R1-R5, C1-C5, V1-V4, G1-G6, etc.); the user refreshes on the G3 and reports green / red per card.

For positioning verification, the file-backed log captures `plot_rect` and `plot_text` calls with exact (x,y) coordinates. Math against expected positions confirms whether a CSS feature is correctly applied — e.g. "R1-3 at y=267, R1-1 at y=179 → diff=88px = 80 (row 0) + 8 (gap) ✓".

### SheepShaver is a partial test

`/home/patrick/Webs/MAC/sheepshaver/` runs OS 9.0.4 via SheepShaver on Linux, useful for:

- Smoke testing — does the build launch at all, does Carbon init succeed
- Rendering regression checks — does MacTrove render, does var() resolve
- Obvious logic bugs that escape Linux syntax check

It is **not** a substitute for real hardware on:

- Hardware-specific crashes (wheel events, scroll-bar live-track, USB Overdrive interactions)
- Real network behavior (the prefs as shipped have no usable ethernet, so first fetch hangs ~2 min on timeout)
- Timing-sensitive bugs (JIT/coop-scheduler pacing differs from real PPC)

A SheepShaver "passed" never closes a hardware-side gate.

### MacsBug is the last resort

The G3 dev machine has MacsBug installed but the user currently lacks an ADB keyboard, which is required for many MacsBug commands. When a crash is hardware-specific and the Linux-source audit comes up empty (the documented case is the wheel crash: see CLAUDE.md "Wheel crash diagnostic exhaustion"), progress halts until ADB hardware is available for `wh`/`sc`/`ip`/`dm sp` capture.

---

## 5. Reasoning style and gotcha tracking

### CLAUDE.md is load-bearing

The "Known Gotchas" section is dense and growing — every non-obvious failure mode that ate more than ~30 min of debugging time gets a gotcha entry with the symptom, the root cause, and the reference fix. Examples:

- Two-source `dispatch.c` / `s_dispatch.c` requiring synchronized updates (fixes116b)
- `uint8_t` between `int32_t` fields creating non-deterministic padding (fixes117 / fixes151b)
- CW8 PPC `long long` miscompile (fixes113)
- UPP macro override unsafe on CarbonLib (fixes147)
- Live-track scroll CDEF crash on real hardware (fixes159)
- `kEventMouseWheelMoved` not in CarbonLib (fixes140)

When you debug something hard, write the gotcha into CLAUDE.md before closing the round. The next agent reading the file at session start needs it.

### Per-project memory entries

Cross-cutting lessons (workflow preferences, naming conventions, repeated gotcha categories) go in the per-project memory directory at `~/.claude/projects/-home-patrick-Webs-macsurf/memory/`. These persist across conversations and pre-load into context for future sessions. Examples in this project:

- "Never blame stale files" (DIRECTIVE #1 captured as a feedback memory)
- "Fix delivery pipeline" workflow note
- "libcss properties with secondary storage need copy/compose to propagate the companion field"
- "Plotters.c port assumption" — the cast-current-port-to-WindowRef pattern that broke offscreen composite

### Be terse, don't explain to the wall

The user already knows the codebase. End-of-turn summary is one or two sentences. No hedging, no preamble, no recapping what they just told you. State diagnoses directly. If you don't know, say "I don't know" — don't pad.

### Auto mode means ship

When the user says "go" or auto mode is on, pick the next item from the queue and ship it. Don't ask for confirmation on routine work. Course corrections from the user are expected and welcome.

---

## 6. Reference tree

| Need | File / dir |
|---|---|
| Project state, gotchas, ship history | `CLAUDE.md` (root) |
| CSS feature inventory | `CSS_STATUS.md` |
| Architecture (broad) | `docs/macsurf-architecture.md` |
| Window-framework architecture | `docs/research/window-architecture-2026-04-22.md` |
| Code | `browser/netsurf/` (NetSurf fork) + `browser/libcss/` (with MacSurf patches) |
| Mac frontend | `browser/netsurf/frontends/macos9/` |
| Debug log impl | `browser/netsurf/frontends/macos9/macsurf_debug_log.c` |
| Cross-pre-flight toolchain | `/home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc` |
| SheepShaver test env | `/home/patrick/Webs/MAC/sheepshaver/` |
| Proxy test page | `mactrove.com/advanced.html` (lives at `/home/patrick/Webs/MAC/drupal/web/advanced.html`) |
| Outbound tars | `~/Documents/macfiles/` |
| SSH bridge to Mac | `scp -P 2222 -i ~/.ssh/macsurf_push patrick@localhost:...` |
| Log pull from Mac | `scp -P 2222 -i ~/.ssh/macsurf_push patrick@localhost:'Documents/macfiles/MacSurf Debug.log' /tmp/` |
