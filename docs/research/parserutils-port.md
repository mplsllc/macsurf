# parserutils Port Audit (CW8 / C89)

## Status

**Research only, no code changes.** This document is a complete inventory of
what is in the libparserutils source tree and exactly what stands between it
and a clean CodeWarrior 8 / strict C89 / Mac OS 9 build.

## Where the source lives

`browser/libparserutils/`, present in the repo, vendored alongside `browser/netsurf/`.
Not a submodule; the tree is checked in directly. License is **MIT**.

Upstream is the NetSurf project's libparserutils:
`https://git.netsurf-browser.org/libparserutils.git/` (or the GitHub mirror at
`github.com/netsurf-browser/libparserutils`). The version in the tree appears
to be a tagged release from the NetSurf side; the embedded README is the
upstream README verbatim.

**Nothing needs to be fetched.** The source is already where we need it.

## Scope

- **35 source/header files**, 11 public headers in
  `browser/libparserutils/include/parserutils/`, 24 implementation files in
  `browser/libparserutils/src/`.
- **7,025 lines** total across the source tree (not counting tests, docs,
  build scripts).
- **No tests in scope**, `browser/libparserutils/test/` is excluded; we don't
  build the test harness on the Mac.
- **No docs in scope**, `browser/libparserutils/docs/` is doxygen output config.

## File map

### Public headers, `include/parserutils/`

| File | Purpose |
|---|---|
| `parserutils.h` | Umbrella header, includes errors/functypes/types |
| `errors.h` | `parserutils_error` enum, error-to-string helpers |
| `types.h` | Pulls in `<stdbool.h>` and `<inttypes.h>` |
| `functypes.h` | Function pointer typedefs |
| `charset/codec.h` | Charset codec public API |
| `charset/mibenum.h` | MIB enum / charset name lookup API |
| `charset/utf8.h` | UTF-8 helpers |
| `charset/utf16.h` | UTF-16 helpers |
| `input/inputstream.h` | Input stream public API + two inline helpers |
| `utils/buffer.h` | Resizable byte buffer |
| `utils/stack.h` | Generic stack |
| `utils/vector.h` | Generic vector |

### Source files, `src/`

| Subdir | Files |
|---|---|
| `utils/` | `buffer.c`, `errors.c`, `stack.c`, `vector.c`, `endian.h`, `utils.h` |
| `input/` | `inputstream.c`, `filter.c`, `filter.h` |
| `charset/` | `aliases.c`, `aliases.h`, `codec.c` (+ `aliases.inc`, **generated, not in tree**) |
| `charset/encodings/` | `utf8.c`, `utf16.c`, `utf8impl.h` |
| `charset/codecs/` | `codec_impl.h`, `codec_ascii.c`, `codec_8859.c`, `codec_ext8.c`, `codec_utf8.c`, `codec_utf16.c`, `8859_tables.h`, `ext8_tables.h` |

## C99 / CW8 incompatibilities

### 1. `inline` keyword, 9 files, all `static inline` helpers

```
src/utils/endian.h
src/utils/buffer.c
src/input/inputstream.c
src/charset/codecs/codec_ext8.c
src/charset/codecs/codec_ascii.c
src/charset/codecs/codec_utf8.c
src/charset/codecs/codec_utf16.c
src/charset/codecs/codec_8859.c
include/parserutils/input/inputstream.h        ← TWO inline functions in a public header
```

The public `inputstream.h` defines `parserutils_inputstream_peek` and
`parserutils_inputstream_advance` as `static inline` in the header itself
(lines 91 and 151). Every file that includes it needs the `inline` keyword to
either be honored or neutralized.

**Fix shape:** `#define inline` (to nothing) in the prefix file, or use the
existing `-Dinline=` already in the Linux cross-check command in CLAUDE.md.
The functions become regular `static` definitions and CW8 will accept them.

### 2. `//` comments, 10+ files

```
src/utils/{vector,stack,buffer,errors}.c
src/utils/{utils,endian}.h
src/input/{filter,filter.h,inputstream}.c
src/charset/codecs/8859_tables.h
... and most other src/charset/* files
```

CW8 strict C89 rejects `//`. Must be converted to `/* */` before the source
is added to the CW8 project. A scripted `sed` pass on Linux can do this
mechanically; the result has to be added to git so the Mac side never sees the
unconverted form.

### 3. `<stdint.h>` and `<stdbool.h>`, used pervasively

Every header and almost every source file pulls in `<stdint.h>` (for
`uint8_t`, `uint16_t`, `uint32_t`) and `<stdbool.h>` (for `bool`/`true`/`false`).

| File | Usage |
|---|---|
| `include/parserutils/types.h` | `#include <stdbool.h>`, `#include <inttypes.h>` |
| `include/parserutils/functypes.h` | `#include <stdbool.h>`, `#include <stdint.h>` |
| `src/utils/endian.h` | `uint32_t` everywhere |
| `src/charset/codecs/*` | `uint8_t`/`uint32_t` arrays |

**Existing infrastructure:** MacSurf already has both shims:
- `frontends/macos9/shims/stdint.h`, provides `uint8_t`/`uint16_t`/`uint32_t`
  via `MacTypes.h` `UInt8`/`UInt16`/`UInt32`
- The MacSurf prefix file `macsurf_prefix.h` already includes `<MacTypes.h>`
  which gets us `enum { false, true }` for `bool`

These shims should cover libparserutils as long as the include path lists
`frontends/macos9/shims` ahead of the system include path so `<stdint.h>` and
`<stdbool.h>` resolve to our versions.

### 4. `<inttypes.h>`, pulled in by `types.h`, `filter.h`, others

`<inttypes.h>` is C99. CW8 has neither it nor the `PRIu32`-style format
specifiers it provides. libparserutils does not actually use any `PRI*` macros
in the audited code, only the type definitions, which already come from
`<stdint.h>`. A trivial `shims/inttypes.h` that does `#include <stdint.h>` and
nothing else suffices.

**Existing infrastructure:** `frontends/macos9/shims/inttypes.h` already
exists. Audit pending: confirm it forwards to `stdint.h`.

### 5. `%zu` format specifier, debug-only, dead code

Two `fprintf(stdout, "...%zu...")` calls live in
`include/parserutils/input/inputstream.h` (lines 105 and 158). Both are
guarded by:

```c
#ifndef NDEBUG
#ifdef VERBOSE_INPUTSTREAM
    fprintf(stdout, "Peek: len: %zu cur: %u off: %zu\n", ...);
#endif
#endif
```

So unless someone compiles with `VERBOSE_INPUTSTREAM` defined, this is dead
code and CW8 never sees it. We just need to **not** define
`VERBOSE_INPUTSTREAM`. (And to be safe in case the macro leaks in: `%zu` →
`%lu` with a `(unsigned long)` cast, or strip the debug block entirely.)

### 6. `iconv` dependency in `src/input/filter.c`

`filter.c` is the only file with a hard POSIX dependency. It uses `iconv_t`,
`iconv_open`, `iconv`, `iconv_close`, and `<errno.h>`. **All of it is gated
by `#ifndef WITHOUT_ICONV_FILTER`**, the upstream library has a documented
build-time off switch. The README explicitly notes:

> For enhanced charset support, LibParserUtils requires an iconv()
> implementation. If you don't have an implementation of iconv(), this
> requirement may be disabled: see the "Disabling iconv() support" section,
> below.

The disable mechanism is `#define WITHOUT_ICONV_FILTER` (or
`-DWITHOUT_ICONV_FILTER` on the compile line). When defined:

- `<iconv.h>` is not included
- `<errno.h>` is not included (it's only inside the iconv branch)
- The `iconv_t cd` field is excluded from the `parserutils_filter` struct
- All conversion calls fall back to `parserutils_charset_codec_create` /
  `_destroy` (the library's own codec layer, which handles UTF-8/16, ASCII,
  and the ISO-8859 family without iconv)

**This is the cleanest possible CW8 path.** Add `WITHOUT_ICONV_FILTER` to
`macsurf_prefix.h` and `filter.c` becomes plain C89 with no POSIX. The
library still handles every charset MacSurf cares about because the proxy
delivers all upstream content as UTF-8 anyway.

### 7. `aliases.inc`, generated, not in tree

`src/charset/aliases.c` does:

```c
#include "aliases.inc"
```

`aliases.inc` is **not** in the tree. It is generated from `build/Aliases`
(a text table of charset name → MIB enum mappings) by `build/make-aliases.pl`,
a Perl script. The Makefile runs this as part of the normal build.

**Build-system implication:** The `.inc` file must be generated on Linux
before the source is shipped to the Mac. Either:

- (a) Run `perl build/make-aliases.pl` once on Linux, commit the resulting
  `aliases.inc` to the repo, and treat it as a checked-in artifact.
- (b) Add the perl run to the Linux→Mac sync script.

The generated file is text, deterministic, and only changes when the
underlying Aliases table changes (which is rarely, character set IANA
registrations are stable). Option (a) is the simplest path.

The script itself is a few hundred lines of Perl that emits a C header full
of static `const` arrays. It does not need to run on the Mac.

### 8. Cross-platform headers used by libparserutils

The complete `#include` list across all 24 source/header files:

**Standard C89 / will work on CW8:**
```
<assert.h>
<ctype.h>
<stdarg.h>      (only in errors.c — varargs error message formatter)
<stddef.h>
<stdio.h>
<stdlib.h>
<string.h>
```

**Need a shim (already exist or trivial):**
```
<stdbool.h>     ← already shimmed via MacTypes.h enum trick
<stdint.h>      ← shims/stdint.h exists
<inttypes.h>    ← shims/inttypes.h exists, audit pending
```

**Gated by WITHOUT_ICONV_FILTER, will not be included:**
```
<errno.h>
<iconv.h>
```

**libparserutils internal:**
```
<parserutils/...>           ← public headers, in include/
"charset/aliases.h"
"charset/aliases.inc"       ← generated
"charset/codecs/codec_impl.h"
"charset/codecs/8859_tables.h"
"charset/codecs/ext8_tables.h"
"charset/encodings/utf8impl.h"
"input/filter.h"
"utils/endian.h"
"utils/utils.h"
```

## What is NOT a problem

I checked for and did not find:

| Feature | Result |
|---|---|
| `__VA_ARGS__` variadic macros | Not used anywhere |
| C99 designated initializers (`.field = ...`) | Not used |
| For-scope declarations (`for (int i = 0; ...)`) | Not used |
| `restrict` keyword | Not used |
| Compound literals (`(struct foo){...}`) | Not used (grep false positives were `switch` statements) |
| `long long` | Not used |
| Flexible array members (`int arr[];`) | Not used |
| Forward enum declarations | Not used |
| Variable-length arrays | Not used (the only `[expr]` array sizes use `#define`d constants) |
| `__attribute__` / `__builtin_*` | Not used |
| `snprintf` / `vsnprintf` | Not used |

This is unusually clean for a C99-claiming library, libparserutils sticks
very close to C89 in practice and only relies on the C99 type headers and
`inline`/`//` syntax niceties. It is one of the easier libraries in the
NetSurf dependency chain to port.

## Existing MacSurf shim that conflicts

`browser/netsurf/frontends/macos9/parserutils/charset/utf8.h` is a
**header-only stub** that declares its own `parserutils_error` enum and a
handful of `parserutils_charset_utf8_*` function prototypes. It does not
forward to or coexist with the real library headers.

Once the real `libparserutils/include/parserutils/charset/utf8.h` is on the
include path, the stub will conflict (duplicate `parserutils_error` enum
definition). The stub must be deleted as part of the wiring step, with the
real header taking its place via the include path order.

Same situation likely exists for any other `frontends/macos9/parserutils/*.h`
shim, none others currently in the tree, but if more get added during
adjacent work they need to be audited the same way.

## Dependency layering inside libparserutils

For ordering compile units in the CW8 project, the rough internal dependency
graph is:

```
Tier 0 (no internal deps, leaf):
  utils/utils.h          (header-only macros)
  utils/endian.h         (header-only inline helpers)
  errors.c               (just the enum→string lookup)

Tier 1 (depends on Tier 0):
  utils/buffer.c
  utils/stack.c
  utils/vector.c
  charset/encodings/utf8.c
  charset/encodings/utf16.c

Tier 2 (depends on Tier 1):
  charset/codecs/codec_ascii.c
  charset/codecs/codec_8859.c
  charset/codecs/codec_ext8.c
  charset/codecs/codec_utf8.c
  charset/codecs/codec_utf16.c

Tier 3 (depends on Tier 2):
  charset/aliases.c       (includes aliases.inc)
  charset/codec.c          (codec factory; resolves codec_impl entries)

Tier 4 (depends on Tier 3 + iconv-gated branch):
  input/filter.c           (charset filter — iconv branch disabled)

Tier 5 (depends on Tier 4):
  input/inputstream.c      (the public entry point)
```

A first build attempt should add files in tier order so the first thing
that fails is the shallowest possible failure.

## What is required to ship this on the Mac

In total, the porting work breaks down into seven concrete steps. Each one is
a separate change, no code in this document.

1. **Generate `aliases.inc`** by running `perl browser/libparserutils/build/make-aliases.pl`
   on Linux. Commit the resulting `src/charset/aliases.inc` to the repo.

2. **Add `WITHOUT_ICONV_FILTER` to the prefix file**
   (`browser/netsurf/frontends/macos9/macsurf_prefix.h`). This excludes the
   only POSIX dependency and removes `<iconv.h>` and `<errno.h>` from the
   compile.

3. **Strip `//` comments to `/* */`** across all 35 libparserutils files via
   a sed pass. Commit the converted files.

4. **Neutralize `inline` keyword**, either via `-Dinline=` on the CW8 command
   line (the cleanest approach if CW8 supports it the way GCC does) or via
   `#define inline` in the prefix file. Already used in the Linux cross-check.

5. **Audit `frontends/macos9/shims/inttypes.h`** to confirm it forwards to
   `stdint.h` and provides nothing else. If it's empty or wrong, fix it.

6. **Delete the conflicting MacSurf parserutils shim header**
   (`frontends/macos9/parserutils/charset/utf8.h`) and its parent directories
   as part of the wiring commit. Add the real `browser/libparserutils/include`
   to the CW8 project's user search paths so the real headers resolve.

7. **Add the 24 source files to `MacSurf.mcp`** in tier order (see above).
   Add `browser/libparserutils/src` to the user search paths so internal
   `#include "input/filter.h"`-style relative includes resolve.

## What this audit does NOT cover

Out of scope for this document, deferred to follow-ups:

- Actually doing any of the seven steps above (this is research only).
- Verifying that `frontends/macos9/shims/inttypes.h` is correct, need to
  read its contents.
- Verifying `<MacTypes.h>` actually provides every `uintN_t` width
  libparserutils needs (it provides `UInt8`/`UInt16`/`UInt32`/`UInt64`; the
  shim has to map them).
- Whether libhubbub and libcss need anything from libparserutils that this
  audit didn't trace (e.g. private internal APIs).
- The `aliases.inc` regeneration process if the IANA charset table changes
  upstream, long-term maintenance question.
- Whether MacSurf's `nsutils` shim also overlaps with anything libparserutils
  needs.

## Bottom line

libparserutils is **the easy library** in the NetSurf dependency chain.
Almost no real C99 features, one POSIX dependency that the upstream library
itself ships an `#ifdef` to disable, and one build-time generation step that
runs once on Linux. Most of the porting work is mechanical: a sed pass for
comments, a `#define` in the prefix file, a perl run on Linux, and adding
files to the CW8 project.

The hardest part will be **wiring it correctly into the CW8 project file
list and search paths** without breaking the existing MacSurf build. That's
project-file work, not C porting work.

## Files

- Audit subject: `browser/libparserutils/`
- Existing MacSurf parserutils stub: `browser/netsurf/frontends/macos9/parserutils/charset/utf8.h`
- Existing MacSurf shims of relevance: `frontends/macos9/shims/{stdint.h,inttypes.h,stdbool.h}`
- Prefix file to update: `browser/netsurf/frontends/macos9/macsurf_prefix.h`
- CW8 project file to update: `browser/netsurf/frontends/macos9/MacSurf.mcp`
