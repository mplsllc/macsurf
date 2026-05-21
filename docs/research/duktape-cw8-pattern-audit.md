# Duktape 2.7.0, CW8 problem-pattern audit

Single-pass `grep` of `browser/libduktape/duktape.c` against the
patterns CodeWarrior 8 in C89 mode is known to reject.

| Pattern                          | Hits | Action |
|---|---:|---|
| `//` line comments               | 0    | none |
| Bare `inline` keyword            | 34, all in `/* */` block comments | none, no real keyword usage |
| `__func__` predefined identifier | 1, in a comment                 | none |
| C99 designated initialisers      | 0    | none |
| `for (int ...)` for-scope decls  | 0    | none |
| Multi-line `"foo \\` string literals | 21 | none, valid C89 line continuation, CW8 supports |

Result: `duktape.c` is C89-clean. Zero source patches required.

The only CW8-facing concern was Duktape's three inline hint macros
(`DUK_INLINE`, `DUK_ALWAYS_INLINE`, `DUK_NOINLINE`) which the stock
`duk_config.h` resolves to `inline` / `__attribute__((always_inline))`
on detected compilers. CW8 falls through detection. Our `duk_config.h`
override block force-defines all three to empty under
`__MACOS9__ || __MWERKS__`, matching the same approach we use for
libcss/libdom inline helpers (the per-TU prefix already does
`#define inline`, but DUK_ALWAYS_INLINE expands to two tokens so the
prefix alone isn't sufficient).

Pass 2 audit (`gcc -std=c89 -pedantic -Wall -D__MACOS9__`):
**0 errors, 28 long-long-constant warnings**, the latter all from the
`DUK_U64_CONSTANT` macro and acceptable per plan.
