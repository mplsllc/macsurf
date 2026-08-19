# frontends/macos9 — CW8, Carbon, Toolbox, OT

This is the Mac OS 9/Carbon frontend: CodeWarrior 8, C89, Carbon API, Open Transport.
Stub headers under this directory stand in for POSIX/library headers not available on
OS 9. Mac Toolbox headers must always be included before any bool/true/false definitions.

**Stub headers currently here** (verified present): `libwapcaplet/libwapcaplet.h`,
`nsutils/endian.h`, `nsutils/time.h`, `nsutils/base64.h`, `nsutils/unistd.h`, `sys/time.h`,
`sys/types.h`, `shims/iconv.h`, `shims/zlib.h`, `shims/stat.h`, `css/utils.h`. (`dom/dom.h`,
`libcss/libcss.h`, and `parserutils/charset/utf8.h` used to be stubbed here too — those
libraries are now fully ported and live under `browser/libdom`, `browser/libcss`,
`browser/libparserutils`; don't recreate stubs for them here.)

## CW8 / C89 build hazards

- **NSLOG**: `#define NSLOG(cat, level, ...) do{}while(0)` — CW8 supports `__VA_ARGS__` as
  a pre-C99 extension, so category/level are consumed as unevaluated macro parameters. A
  varargs-*function* stub instead evaluates the category token as a real expression, and
  fails with "undeclared identifier" anywhere that token (`fetch`, `llcache`, `layout`,
  `flex`, `schedule`, ...) isn't in scope as a variable. For the same reason, **never
  `#define` an NSLOG category token** — those names collide with real
  variables/parameters/struct members elsewhere in the codebase.
- A **named enum declared inside a struct body** (`struct foo { enum bar {...} type; };`)
  leaves the struct incomplete under CW8 C89. Hoist the enum above the struct. (Anonymous
  enums inside a struct body are fine.)
- **Shim headers must claim the SAME include guard as the real header they shadow.** If a
  shim uses its own guard, both the shim and the real header can get processed in the same
  translation unit once the real one is reachable via a different access path, causing
  "illegal name overloading."
- **A shim that sets a real library's include guard WITHOUT defining its structs silently
  starves every TU that includes it.** CW8's access-path order puts this directory before
  the real one, so a shim that claims the guard but not the content wins and the real
  header never processes. Fix pattern: make the shim a forwarder with its OWN guard
  (`MACSURF_SHIM_*`) that `#include`s the real header by its full path.
- **CW8 cannot resolve a relative `#include "sub/foo.h"` from within a header reached via
  an access path.** Same-directory `#include "foo.h"` works; a path prefix generally
  doesn't. Keep cross-directory includes to files that are themselves entry points, not
  headers included via search paths.
- **`AlwaysSearchUserPaths=true` + case-insensitive HFS means any same-named header
  anywhere on an access path can silently shadow the intended one.** If a system header's
  content looks wrong or a symbol you expect is missing, check what's *actually* being
  included (case-insensitively, across every access path) before assuming the header is
  broken.
- **Suppress an unwanted `<Carbon.h>` sub-header by pre-defining ITS OWN include guard**
  before `#include <Carbon.h>` (see `macos9.h`'s `__INTERNETCONFIG__` / `__KEYCHAINCORE__`
  / `__ATSLayoutTypes__` defines). Chasing the actual problematic sub-header (C11 anonymous
  members, an AliasHandle-by-value struct field, etc.) is a dead end when MacSurf never
  calls into that API anyway.
- **`NETSURF_LOG_H` is pre-defined in the prefix** to block `log.h`'s GNU-attribute NSLOG
  macros — but `log.h` also carries the `nslog_ensure_t` typedef that `log.c` needs, so the
  prefix defines that typedef itself rather than relying on the suppressed header.
- **`mac_dirent.h` is injected globally by the prefix** and claims guard `MAC_DIRENT_H`.
  Any TU-local `dirent.h` shim for the same types must reuse that SAME guard, not a new
  one, or `struct dirent` gets redefined in the same TU.
- **`int *` and `int32_t *` (== `long *`) are incompatible pointer types on CW8 PPC.** Keep
  a shared function's signature and every caller consistent (prefer `int32_t *`).
- **Any header that uses `nserror` in a function-pointer-typed struct member must
  `#include "utils/ns_errors.h"` itself** — don't rely on include order from callers to
  supply the typedef first.
- **`'carb'` resource in `MacSurf.rsrc` is mandatory.** Without it CFM treats the binary as
  classic PEF, CarbonLib never engages, and any `*InContext` OT call crashes at a fixed
  address inside OTClientLib.
- **`kInitOTForApplicationMask` isn't defined in CW8's OT headers** — define it by hand
  (`0x00000002`) where needed. `<OpenTransport.h>` itself is safe to include directly.
- **Carbon event classes have per-CarbonLib-version availability — check Apple's
  `CarbonEvents.h` annotations before installing a handler.** `kEventMouseWheelMoved` was
  never back-ported to CarbonLib and destabilizes the Carbon event dispatcher if you try to
  register for it anyway (illegal-instruction crash, not a clean failure). Current wheel
  handling lives in `main.c`: no native Carbon wheel handler, scroll via bar/keyboard only.
- **`kWindowStandardHandlerAttribute` intercepts update events and leaves windows blank.**
  Never pass it to `CreateNewWindow`.
- **`TENew`/`TEDispose` crash with `dsMemWZErr` unless `SetWRefCon(window, 0)` runs before
  the first `TENew` call on a fresh window.**
- **TextEdit fields need explicit `TEActivate` on window activation and `TEIdle` on every
  null event** for the caret to blink and the field to accept keystrokes. Gate `TEKey`
  behind a `url_field_active`-style flag so Return/Escape don't get routed as typed
  characters.
- **Never call `browser_window_schedule_reformat` synchronously from a resize/grow-box
  handler.** Set a `needs_reformat` flag and drain it on the next null event pass, guarded
  by a `reformat_in_progress` re-entrancy flag, or you get an infinite layout loop.
- **UPP macro overrides that cast a raw function pointer to a UPP are unsafe on CarbonLib
  (OS 9), even though the same pattern is fine on Mach-O Carbon (OS X).** CarbonLib's
  MixedMode expects a RoutineDescriptor, not a bare pointer; dispatching through one
  crashes deep in low memory (PC near `0x8`, LR near `0x4`). Use
  `TrackControl(ctrl, pt, NULL)` and read the resulting part code back instead of an action
  UPP.
- **The Appearance live-tracking scroll bar CDEF (`kControlScrollBarLiveProc`, proc 386)
  crashes on real G3/G4 hardware and is NOT reproducible in SheepShaver.** Use the
  non-live CDEF (`kControlScrollBarProc`, proc 384) and read the final value with
  `GetControlValue` on return; the trade-off is no live-scroll during a thumb drag.
- **Carbon partition must be at least 16 MB preferred.** libcss allocates via raw
  `malloc`/`calloc` with no wrapper — below that floor it returns `CSS_NOMEM` mid-cascade
  on a real page.
- **CW8 PPC miscompiles `(long long)a * small_const`** — the multiply-by-constant codegen
  writes the wrong bits into the high word. Route 64-bit fixed-point math through `double`
  under `#ifdef __MWERKS__` (see `browser/libcss/include/libcss/fpmath.h`) rather than trusting `long long`
  arithmetic on this target. Plain int32 multiply/divide is fine; this is specifically the
  64-bit shift-multiply path.

## Networking / fetchers

- **A custom fetcher must call BOTH `fetch_remove_from_queues(handle)` and
  `fetch_free(handle)` after every terminal callback** (`FETCH_FINISHED`/`ERROR`/
  `REDIRECT`). Calling only `fetch_free` is worse than calling neither — the struct is
  freed but stays in `fetch_ring` as a dangling pointer that the next `RING_GETSIZE` walks
  into freed memory. Check any `ops.abort` deferred-cleanup flag BEFORE a state-based
  early-return in the poll loop, or an aborted-while-queued fetch never leaves the ring.
  **A different root cause for the same "page hangs / never finishes loading" symptom** —
  a fetch that aborts without calling `content_broadcast_error` — is filed in
  `browser/netsurf/content/CLAUDE.md` → "Fetch lifecycle".

## Rendering / plotting

- **`plotters.c` must never assume the current QuickDraw port is the window** when
  resolving a `struct gui_window *`. Anything that does `SetGWorld()` mid-redraw breaks the
  naive `GetPort` + `GetWRefCon` pattern (the GWorld pointer gets cast as a WindowRef and
  `GetWRefCon` reads garbage). Use `macos9_find_gw_for_plot()` — checks the
  `macos9_paint_gw` global first, falls back to `GetPort`+`GetWRefCon` — at every clip/plot
  call site.
- **`CopyBits`/`CopyMask` colorize the transfer with the port's current fg/bg color.** Reset
  `RGBForeColor(black)` + `RGBBackColor(white)` immediately before any QuickDraw blit
  unless you deliberately want colorizing, or an image tints toward whatever text color was
  drawn last (dark images go blue, bright images wash out).

## Diagnostics

- **Log lines must be prefixed `LIFE ` to survive the release build's failures-only
  filter** (the gate matches literal strings/prefixes, not a general severity level) — a
  new diagnostic without it is silently dropped from `MacSurf Debug.log` and looks like the
  code never ran.
- **The session-banner build timestamp (`__DATE__`/`__TIME__` in `macsurf_debug_log.c`)
  freezes to whenever THAT FILE was last compiled, not the build as a whole.** A fix round
  that doesn't touch `macsurf_debug_log.c` leaves the banner stamped with an old
  date/time even though every other shipped file is current. Never use the banner to judge
  whether a fix is in the build — look for that fix's own log line instead.
- `MS_LOG` is defined in `macsurf_debug.h` (the `_init`/`_close`/`_write`/`_writef` API
  itself lives in `macsurf_debug_log.h`).
- **The on-disk cache (`macos9_disk_cache.c`) streams a fetch body straight to disk and is
  checked before the network in both fetchers.** Never buffer a body in RAM purely to write
  it to cache afterward — the buffer, not the file I/O, is what blows the heap budget on a
  128 MB partition.
- **Title-bar diagnostic probes (`macsurf_debug_set_title_force`) clobber each other** —
  last writer in a reformat cycle wins. Strip a predecessor's sticky probe before adding a
  new one, or you'll read the wrong signal and chase the wrong code path.
