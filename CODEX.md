# Codex Notes For MacSurf

Read this after `CLAUDE.md`. This file is my Codex-facing memory for this repo: short,
operational, and biased toward avoiding repeated mistakes. If this conflicts with
`CLAUDE.md` or a subsystem `CLAUDE.md`, the more specific/newer `CLAUDE.md` wins.

## Project Truth

- MacSurf is a Classic Mac OS 9 / early Carbon browser forked from NetSurf.
- Current networking is direct native HTTPS through `macTLS` with TLS 1.3 and TLS 1.2
  fallback. Do not reintroduce or describe a TLS-stripping proxy as the current path.
- Current JavaScript is QuickJS/macQJS (ES2023), not Duktape. Some older docs still say
  Duktape or proxy; treat those as history unless the code confirms otherwise.
- Target remains real PowerPC G3/G4 hardware on Mac OS 9.1-9.2.2 with CodeWarrior 8.3.
  SheepShaver is useful for smoke tests, not hardware verification.
- Tabs are unimplemented. Multi-window is real.

## Session Preflight

At session start and before every ship/commit, run the project hygiene checks from
`CLAUDE.md`:

```sh
git status --short
git ls-files --others --exclude-standard '*.c' '*.h'
git fetch --all -q
```

Then check each remote for divergence:

```sh
git log --oneline master..REMOTE/master
git log --oneline REMOTE/master..master
```

The untracked `.c`/`.h` check must be empty. A C/H file in the build but not in git is a
fresh-clone build break waiting to happen. Do not clean, revert, or normalize user changes
unless explicitly asked.

## Non-Negotiables

- Never suggest stale Mac files, stale object code, incomplete rebuilds, failed unpacking,
  or a silent transfer failure. The user's hardware build is the build they say it is.
  Diagnose through the Linux source and ship a code-side fix.
- Never close issues, mark a fix verified, tag `*-verified`, or update trackers as done
  from local/synthetic tests. Use: `shipped -- awaiting your verification on <reported case>`.
- Do not edit or ship `MacSurf.mcp`. If new `.c` files are needed, list them in the handoff
  for the user to add in CodeWarrior. After adding them, request a normal rebuild. Do not
  recommend "Remove Object Code" routinely; reserve it for a demonstrated stale-build issue.
- When shipping a fix, provide only the drop plus:
  1. files to add/remove from `MacSurf.mcp`, if any
  2. access paths to add/remove from `Access Paths.xml`, if any
- Keep C source strict C89/CW8-safe: declarations at block top, no `//`, no `inline`, no
  designated initializers, no for-scope declarations, no compound literals, no forward enum
  declarations, no union casts, and avoid `long long` fixed-point math on CW8.

## Build And Test Handles

- Isolated C89 syntax check:

```sh
gcc -fsyntax-only -std=c89 -pedantic -Dinline= \
  -Ibrowser/netsurf/frontends/macos9/shims \
  -Ibrowser/netsurf/frontends \
  -Ibrowser/netsurf/include \
  -Ibrowser/netsurf \
  -include stdbool.h <file>
```

This only works for files that do not reach `macos9.h`; Carbon headers are not available on
Linux.

- Harness checks live under `harness/`; use `make check-c89` and `make check-macdefault`
  before drops touching `MACSURF_JS_*` switches.
- Never add `-w` to harness targets. It can hide the warning the gate exists to catch.
- Current delivery path in `CLAUDE.md`: `./forclaude/drop-to-imac.sh <fixnum> <paths...>`.
  It handles CR line endings, future mtimes, scp, and remote verification. Do not hand-roll
  CR conversion or mtime stamping.
- `tools/ship_fix.sh` is a retired fallback.

## Subsystem Memory

- Root rules and cross-cutting gotchas: `CLAUDE.md`.
- CSS/libcss: `browser/libcss/CLAUDE.md`.
- Fetch/parse/layout/redraw pipeline: `browser/netsurf/content/CLAUDE.md`.
- Mac OS 9 frontend, Toolbox, Carbon, Open Transport: `browser/netsurf/frontends/macos9/CLAUDE.md`.
- QuickJS glue: `browser/netsurf/frontends/macos9/javascript/CLAUDE.md`.
- TLS stack: `macTLS/CLAUDE.md`.
- Build/C89 explanatory docs: `docs/build/cw8-c89-gotchas.md`, `docs/build/building-macsurf.md`.
- Current status and roadmap: `docs/status.md`, but prefer the newest `.private/research/css-gap-inventory-*.md`
  over stale CSS inventories when planning CSS work.

## High-Risk Gotchas To Keep Loaded

- `TARGET_API_MAC_CARBON` must be defined before the first `<MacTypes.h>` include.
- The `'carb'` resource in `MacSurf.rsrc` is mandatory.
- No preemptive threads; OS 9 is cooperative. Networking yields through Open Transport idle
  events and the UI loop is `WaitNextEvent`.
- QuickDraw blits colorize through current fg/bg; set black/white before `CopyBits` or
  `CopyMask`.
- CarbonLib on OS 9 expects real MixedMode UPP routine descriptors, not raw PPC function
  pointer casts.
- Carbon event availability matters. If `CarbonEvents.h` says `CarbonLib: not available`,
  do not register that event handler.
- `css_computed_style_i` is raw-`memcmp` interned. Scalar fields must be deterministic and
  self-aligning; prefer `int32_t` at the end. Variable-size data belongs on the outer
  `css_computed_style` with logical arena comparison.
- CW8 PPC miscompiles some `long long` multiply-by-constant fixed-point math; route through
  `double` under `__MWERKS__` where needed.
- Shim headers must either forward correctly or match the real include guard exactly. Bad
  guards cause duplicate definitions or silently starve the real header.
- `int32_t *` is `long *` on PPC and is not compatible with `int *`.
- Log lines need `LIFE ` to survive the release build's failures-only filter.
- A fetch that aborts must still complete the NetSurf lifecycle; otherwise pages hang with
  an active fetch count that never reaches zero.

## Current Open-Work Shape

From the current docs, the valuable next work tends to be:

- `justify-self` as a real native libcss property, not just a `text-align` bridge.
- `background-clip: text` and `background-origin`.
- Grid Round 2: placement/span-aware auto sizing, `minmax()`/`fit-content()` behavior,
  stretch defaults, FR-row distribution.
- `caption-side` and `list-style-position` layout consumers.
- Intrinsic sizing keywords for height/flex/grid/table paths.
- Reconvert crash chain before re-enabling JS DOM mutation repaint.

Confirm against current code and the newest private inventory before choosing a task.

## Communication Style For This Repo

Be terse and concrete. The maintainer knows the system. If a fix ships, state what changed,
what was shipped, and what needs Mac-side project/access-path changes. No lectures about
rebuild hygiene, no staleness theories, no synthetic-test victory laps.
