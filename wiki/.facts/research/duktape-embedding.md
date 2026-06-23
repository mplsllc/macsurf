# Duktape Embedding — Research Notes

Duktape is a small, embeddable JavaScript (ECMAScript) engine written in C, with
a deliberate focus on portability and a compact footprint. It is designed to be
dropped into a C/C++ host program and driven through a Lua-style C API. This is
the engine MacSurf links into its base build (Duktape 2.7.0) for in-browser ES5
script evaluation. The notes below are drawn from the official Duktape site,
the project's GitHub repository, the Programmer's Guide, and the portability wiki.

## What it is

- Duktape is "an embeddable Javascript engine, with a focus on portability and
  compact footprint." — https://duktape.org/
- It targets **ECMAScript E5/E5.1** ("with some semantics updated from
  ES2015+"), plus partial ES2015 (E6) and ES2016 (E7) features. It is not a
  modern full-ES2015+ engine. — https://duktape.org/ and https://github.com/svaarala/duktape
- Licensed under the **MIT license** (LICENSE.txt in the repository). —
  https://github.com/svaarala/duktape
- Latest stable release of the 2.x line is **2.7.0, released 2022-02-19**. The
  `master` branch carries in-progress incompatible 3.x changes; the
  `v2-maintenance` branch tracks the stable 2.x series. —
  https://github.com/svaarala/duktape
- The C API is "similar in spirit to Lua's" — a value stack plus a context
  handle passed to nearly every call. — https://duktape.org/ and https://duktape.org/guide.html

## How it embeds into a C host

- You add **three files** to your build: `duktape.c`, `duktape.h`, and
  `duk_config.h`. No external library or link step beyond these. —
  https://duktape.org/ and https://github.com/svaarala/duktape
- **Heap lifecycle**: create with `duk_create_heap_default()` (default
  allocators/fatal handler) or `duk_create_heap(alloc, realloc, free, udata,
  fatal)` for custom memory functions; destroy with `duk_destroy_heap(ctx)`,
  which frees all heap objects and invalidates the context pointer. —
  https://duktape.org/guide.html
- A **`duk_context *`** represents an ECMAScript execution thread within a heap
  and is the handle passed to almost every API call; it gives access to the
  **value stack** (an array of tagged values; index 0 is the bottom, negative
  indices count from the top). — https://duktape.org/guide.html
- **Evaluating scripts**: `duk_eval_string(ctx, code)` evaluates and throws on
  error; `duk_peval_string(ctx, code)` does *protected* evaluation and returns
  an error code instead of throwing; `duk_eval_string_noresult(ctx, code)`
  evaluates without leaving a result on the stack. — https://duktape.org/guide.html
- Features include a combined **reference-counting + mark-and-sweep garbage
  collector** with finalization, **coroutines**, a built-in regex engine,
  Unicode support, bytecode dump/load, and a debugger protocol. —
  https://github.com/svaarala/duktape

## Portability and constrained/old platforms

- Footprint claim: Duktape "can run on platforms with **160kB flash and 64kB
  RAM**." — https://duktape.org/
- Known-working platforms include exotic/retro ones such as **AmigaOS and RISC
  OS** alongside Linux/Windows/macOS/Android; the list is "what is known to
  work," not exhaustive. — https://wiki.duktape.org/portability
- Hardware assumptions: **two's-complement signed arithmetic** and **IEEE
  floating-point** behavior; both 32-bit and 64-bit (x86, ARM, MIPS, etc.) are
  supported. — https://wiki.duktape.org/portability
- **C99 is strongly recommended but not strictly mandatory.** Without C99, type
  detection is less reliable. (This is the relevant caveat for a strict-C89
  toolchain like CW8 — Duktape can compile pre-C99 but you lean harder on
  duk_config.h to pin types/byteorder yourself.) — https://wiki.duktape.org/portability

## duk_config.h (the portability header)

- `duk_config.h` is an **external configuration header** providing all platform,
  compiler, and architecture-specific features, kept separate from the main
  source so it can, as a last resort, be hand-edited or written from scratch for
  exotic platforms. — https://github.com/svaarala/duktape/blob/master/doc/duk-config.rst
- It carries `DUK_USE_*` options. Verified examples from the docs:
  endianness via `DUK_USE_BYTE_ORDER`, alignment via `DUK_USE_ALIGN_BY`, plus
  feature toggles like `DUK_USE_FASTINT`, `DUK_USE_JX`,
  `DUK_USE_BUFFEROBJECT_SUPPORT`. — https://github.com/svaarala/duktape/blob/master/doc/duk-config.rst
- Since **Duktape 2.0**, `tools/configure.py` is the recommended way to generate
  a config header plus prepared sources for a custom configuration. —
  https://github.com/svaarala/duktape/blob/master/doc/duk-config.rst
- Run-time self-tests can run at heap creation to catch platform/compiler
  problems (e.g. endianness, alignment) that can't be reliably detected at
  compile time. — https://wiki.duktape.org/portability

### Uncertain / not verified

- I could **not confirm from a fetched page** the exact Duktape version that
  first externalized portability defines into `duk_config.h`; secondary search
  text said "since 1.3," but the duk-config doc I fetched did not state the
  version. Treat the "1.3" figure as unverified.
- The packed-value option `DUK_USE_PACKED_TVAL` is referenced in MacSurf's own
  hand-built config (per project notes) but I did **not** see it enumerated on
  the pages I fetched; the duk-config doc explicitly did not list it. Its
  existence is plausible but unverified here.
- `DUK_OPT_FORCE_BYTEORDER` / `DUK_OPT_FORCE_ALIGN` appear in search summaries as
  *legacy* (pre-1.3 `DUK_OPT_*` style) options; the modern equivalents are the
  `DUK_USE_*` names above. Don't mix the two families.

## Beginner gotchas / things that surprise people

- **It is ES5, not modern JS.** No native `let`/`const`/arrow-function/Promise
  guarantees — only partial ES6/ES7. Code written for a current browser will
  often fail to parse. — https://duktape.org/
- **Use the *protected* eval (`duk_peval_string`) for untrusted input.** Plain
  `duk_eval_string` throws a C-level Duktape error (longjmp) that, if uncaught,
  reaches the fatal handler and aborts; `duk_peval_string` returns an error code
  instead. — https://duktape.org/guide.html
- **Everything is a value stack.** Results of eval/calls are left on the stack;
  forgetting to pop them, or using the wrong (relative vs. negative) index, is a
  classic first-time mistake. — https://duktape.org/guide.html
- **Pre-C99 compilers are second-class.** Duktape *can* build without C99, but
  automatic type/endianness detection degrades, so on old toolchains you must
  pin types and byte order in `duk_config.h` yourself. —
  https://wiki.duktape.org/portability
- **The single-file `.c` is an amalgamation.** `duktape.c` is large; that is
  expected (it's a combined source), not a packaging error.

## Sources

- https://duktape.org/ (official site / overview)
- https://duktape.org/guide.html (Programmer's Guide — embedding, heap, eval)
- https://github.com/svaarala/duktape (project repository — version, license, features)
- https://github.com/svaarala/duktape/blob/master/doc/duk-config.rst (duk_config.h documentation)
- https://wiki.duktape.org/portability (portability requirements — C99, endianness, alignment)
