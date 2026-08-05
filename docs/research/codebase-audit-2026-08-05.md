# MacSurf Codebase Architecture Audit — 2026-08-05

**Branch:** `cleanup` (HEAD `533da123`) — **17 cleanup commits in last 200.**
**Scope:** `browser/netsurf/frontends/macos9/` (28 `.c` files, 21,853 lines in 6 largest files)

---

## 1. Executive Summary

The macos9 frontend is **not AI-generated**. There are no classic AI tautologies ("`// increment x`" above `x++`"), no commented-out dead code, no hallucinated functions. The code is hand-maintained by a solo developer with strong opinions, and every stub/forwarder/no-op carries a `fixesNNN` rationale comment.

The technical debt is **organically grown**: 1,861 `fixesNNN` marker comments across the frontend document a history of guarded-block accretion — each fix added an `if`/`case`/`#ifdef` block into an existing god function rather than refactoring. The result is two massive files (11,497-line `macsurf_qjs.c`, 3,666-line `macos9_tls_fetcher.c`), 35% code duplication between the two fetchers, 9-level-deep conditional nesting in mouse/scrolling handlers, and 160+ mutable non-static globals.

**Grade:** C+ for structure, A for correctness-awareness. The code knows its own warts and documents them honestly.

---

## 2. AI-Generated Cruft: NONE FOUND

### 2.1 Tautological Comments — CLEAN

The frontend `.c` files contain only 71 `//` line comments (CW8 C89 compliance) and ~3,500 `/* */` comment blocks. Nearly all are substantive design notes with `fixesNNN` references. The few mild restatements found:

| File | Line | Issue |
|---|---|---|
| `window.c` | 365, 368 | `/* top highlight */` / `/* bottom shadow + separator */` restate adjacent assignments |
| `window.c` | 1096, 1105, 1107 | `/* Trim trailing spaces. */` restates following loop |
| `schedule.c` | 299, 302 | `/* Negative t means... */` restates guard clause |
| `macos9_http_fetcher.c` | 306 | `/* Compact: move tail entry... */` restates assignment |

**Verdict:** Not worth fixing — these are <10 mild cases in 22,000 lines.

### 2.2 Commented-Out Code — NONE

Zero `#if 0` blocks, zero `//`-prefixed executable statements, zero `/* */` comment blocks containing dead code in the frontend. The only code-in-comments is in `macsurf_qjs.c` (lines 6160, 6335, 8136-8159, 8243, 8398, 8695), where minified JS library source is preserved as reference documentation inside block comments — deliberate, not stale.

### 2.3 Hallucinated/Dead Functions — NONE

Every no-op body found is intentional and documented:

- `misc_stub.c:28` `void netsurf_poll(void) {}` — cooperative loop makes it a no-op
- `js_stub.c:30-31, 111-113, 119, 124` — all `{}` no-ops inside `#ifndef WITH_QUICKJS` guard (always dead, kept for build-link compatibility)
- `macos9_desktop_stubs.c` — entire 58-line file of documented no-op hooks for unbuilt desktop modules
- `macos9_http_fetcher.c:1518-1519` `macos9_http_initialise`/`_finalise` — standard NetSurf fetcher contract

### 2.4 Excessive Comment Density — REAL, BUT CONTEXTUAL

12 files have >25% comment density. The worst offenders:

| File | Comment % | Notes |
|---|---|---|
| `macos9_wheel.c` | **73%** | 52/71 lines — entire file is a historical record of a retired feature |
| `macsurf_lwc_compat.c` | **57%** | 26/45 lines — a 2026-08-05 extraction with full provenance essay |
| `macos9_reconvert.c` | **49%** | 381/776 lines — 17-line essay for a `#define`, 27-line essay for an 8-entry array cap |
| `macos9_deathrow.c` | **49%** | 161/326 lines — deferred-destruction registry with full design rationale |
| `macsurf_timebase.c` | **43%** | NanoKernel timebase detection, hardware-specific |
| `macsurf_memory.c` | **39%** | Heap-bounds detection, anti-UAF layer |

**Assessment:** The extreme-comment files are the **hardest-won subsystems** — every one of them documents a crash that was diagnosed through hardware MacsBug traces. The comments are the forensic record. Reducing them is low-value; what they signal is that the subsystem they document should be **extracted to its own file** (several already were — `macsurf_lwc_compat.c` was extracted 2026-08-05).

---

## 3. Spaghetti Logic Mapping

### 3.1 The God File: `macsurf_qjs.c` (11,497 lines)

**Metrics:**
- 230 functions (94 internal entry points, 24 non-static, remainder static)
- 673 `if` statements, 9 `switch` blocks with 49 `case` labels
- 51 file-scope static variables, 45 `extern` declarations
- 475 `fixesNNN` markers — **2.5× the next-worst file**
- 30 preprocessor conditionals (all depth 1, mostly `#ifdef __MACOS9__`)

**Structural problems:**
1. **`register_browser_globals()` — 1,417 lines (7943-9359).** Registers hundreds of JS globals with inline string evals. This is a function that grew by appending — every new JS API binding was added as another block. Should be a table-driven registration (`{name, value_or_fn, flags}[]`).
2. **Mid-file includes** at lines 1901 and 9395 — `#include "macos9_reconvert.h"` and `content/content_protected.h` placed mid-file to satisfy late-added code rather than restructured.
3. **`macsurf_qjs_page_js_summary()` — 518 lines (10582-11099).** Diagnostic summary that touches every JS counter in the file; should be in its own `_audit.c` companion.
4. **`qjs_dom_listener_cb()` — 302 lines.** The DOM event dispatch callback with per-type switching — a 30-case switch inside a function.
5. **`js_destroythread()` — 332 lines.** Teardown of JS contexts touching 15+ subsystems.

**Root cause:** `macsurf_qjs.c` is the **integration hub** — it bridges QuickJS to DOM, layout, fetch, timers, events, geometry, CSS computed style, cookies, console, and audit/diagnostics. Every one of those subsystems grew by accretion. The file knows it: the 475 fix markers document each addition.

### 3.2 The Fetcher Duplication: 35% Shared Code

`macos9_http_fetcher.c` (1,913 lines) and `macos9_tls_fetcher.c` (3,666 lines) share ~441 lines of verbatim or near-verbatim code:

| Shared block | HTTP line | TLS line | Description |
|---|---|---|---|
| Cache accumulator | 194-244 | 1049-1093 | Identical doubling-realloc, same overflow latch |
| Cache-hit replay | 1242-1280 | 2773-2810 | Same synthetic-Content-Type, same one-shot body delivery |
| Line finder | 372-380 | 1821-1834 | Same CRLF-NUL contract |
| Header parser | 902+ | 1842+ | Same line-emission structure; **copy-pasted 12-line `fixes641` bug story** identical in both |
| Connection pool | 297-371 | 828-924 | Same scan-backwards / compact-tail pattern |
| Setup path | 405 | 2560 | Same disk-cache guard, cookie jar pull, UA fetch, request template |
| Struct fields | 112-167 | 184-268 | 10+ identically-named fields with identical comments |

The recent cleanup commits on the `cleanup` branch (`533da123`, `9b59b481`, `854233ae`) have already begun addressing this — shared request-header helpers and buffer-size constants were extracted. The cache accumulator, cache-hit replay, and header parser remain the three largest duplication targets.

### 3.3 Deepest Nesting (5-9 levels)

| Function | File | Lines | Max Depth | Mechanism |
|---|---|---|---|---|
| `macos9_handle_mouse_down` | `main.c:919-1243` | **325** | **8-9** | `switch`→`case`→`if`→`if`→`while(StillDown())`→`if(dragging)`→`if(edge)`→`if(time)` |
| `macos9_window_handle_scrollbar_click` | `window.c:514-586` | 73 | **7-8** | `switch`→`case`→`while(StillDown())`→`if`→`if(span)`→`if(val)`→bare`{}`→`if(time)` |
| `process_chunked_bytes` | `macos9_http_fetcher.c:808-900` | ~92 | **6-7** | Hand-rolled chunked-transfer state machine — `while`→`switch(CS_*)`→`while`→`if`→`if(sz)`→`else` |
| `qjs_sel_parse` | `macsurf_qjs.c:6799-6947` | **149** | **7-8** | Hand-written CSS selector parser — `for`→`else if`→`while`→`if`→`while` |

**Both mouse handlers implement the SAME drag-tracking pattern** (poll `StillDown()` with manual throttling) in different files at the same nesting depth — duplicated logic, not just duplicated code.

### 3.4 Global State Proliferation

**160+ non-static mutable globals** across the frontend. Top hoarders:

| File | Non-static globals | + Static globals | Total |
|---|---|---|---|
| `macsurf_debug_log.c` | 44 | 34 | 78 |
| `window.c` | 25 | 38 | 63 |
| `plotters.c` | 16 | ~30 | ~46 |
| `macsurf_qjs.c` | ~8 | 51 | ~59 |
| `macos9_tls_fetcher.c` | 7 | 11 | 18 |

**Worst pattern:** `macsurf_http_skip_next_cache` (`macos9_disk_cache.c:62`) — a single `int` flag mutated by http_fetcher, tls_fetcher, disk_cache, and window.c across 4 files, declared `extern` in each consumer. This is the C equivalent of a global mutex with no owner.

**The diagnostic counters are mostly fine** — `macsurf_debug_log.c`'s 44 globals are single-writer diagnostic accumulators. The problem is the **mutable control state scattered across files**: `macos9_quitting`, `macos9_done`, `macos9_ot_context`, `macos9_hittest_scroll_x/y`, `macos9_paint_gw`, `g_ostls_ot_context`.

### 3.5 Accretion Intensity: Fix Markers as Technical Debt Proxy

| File | Fix markers | Key functions >200 lines |
|---|---|---|
| `macsurf_qjs.c` | **475** | 5 (including a **1,417-line** god function) |
| `macos9_tls_fetcher.c` | **220** | 4 (including a **634-line** `hctx_poll`) |
| `main.c` | 154 | 3 |
| `window.c` | 149 | 0 (but 73-line scrollbar at depth 8) |
| `plotters.c` | 148 | 0 |
| `macos9_http_fetcher.c` | 138 | 2 |

**The fix-marker count correlates strongly with function length, nesting depth, and global count.** Each marker represents a guarded block added inline rather than a refactor. At 475 markers, `macsurf_qjs.c` is the most extreme case — it has been modified once every ~24 lines on average.

---

## 4. Legacy Constraint Audit

### 4.1 CW8 C89 Compliance — GOOD

- **`//` comments:** Only 71 in frontend `.c` files (all intentional single-line notes in otherwise `/* */` files)
- **`inline` keyword:** Stripped by `macsurf_prefix.h` (`#define inline`)
- **C99 designated initializers:** Converted across all 443 library files during porting (documented in CLAUDE.md §"Library port audit checklist")
- **`for (int i...)`:** Converted to block-scope declarations
- **Variadic macros:** `__VA_ARGS__` used ONLY for NSLOG (documented CW8 extension)
- **`long long`:** Routed through `double` on CW8 PPC (`fpmath.h` — documented miscompile workaround)
- **`restrict`:** Not found in frontend code

### 4.2 Multi-threading — N/A (Correct)

Zero pthread/mutex/std::thread references in the frontend. OS 9 is cooperative; the code correctly uses `WaitNextEvent` + `YieldToAnyThread()` in OT callbacks.

### 4.3 Modern Standard Library — CLEAN

- `#include <stdint.h>` is included but only in `misc_stub.c` and shim headers — CW8-compatible
- `snprintf`/`vsnprintf` — `macsurf_debug_log.c` uses a hand-rolled formatter specifically because MSL's `vsnprintf` is unreliable on CW8 Carbon MSL
- `%zu`/`%zd` format specifiers — not found in frontend code
- POSIX-only APIs (`mmap`, `fork`, `exec`, `pipe`, `socket`) — not found

### 4.4 Shims Layer — ADEQUATE

21 shim files provide POSIX equivalents. Coverage is complete for what the code uses:
- `mac_time.c` (342 lines) — time operations
- `mac_file_io.c` (253 lines) — file I/O
- `mac_iconv.c` (236 lines) — charset conversion
- `mac_stat.c` (143 lines) — stat()
- `mac_dirent.c` (167 lines) — directory iteration

### 4.5 Preprocessor Abuse — MINIMAL

No deeply nested `#ifdef` chains (max depth 2). The pattern is whole-function `#ifdef __MACOS9__` wrappers — clean but suggests some functions could be platform-split at file granularity.

### 4.6 Header Duplication — ONE CASE

`time.h` exists at both:
- `frontends/macos9/time.h` (frontend shim)
- `netsurf/include/nsutils/time.h` (core NetSurf)

Different purpose (frontend shim for CW8 vs core nsutils), but same basename — a CW8 access-path footgun.

---

## 5. Top-10 Most Actionable Findings (Ranked)

| # | Finding | Severity | Effort | File(s) |
|---|---|---|---|---|
| 1 | **Fetcher cache code duplicated 35%** — 441 lines of near-verbatim code between http/tls fetchers | High | Medium | `macos9_http_fetcher.c`, `macos9_tls_fetcher.c` |
| 2 | **`register_browser_globals` is 1,417 lines** — JS global registration by accretion | High | High | `macsurf_qjs.c:7943-9359` |
| 3 | **`macsurf_http_skip_next_cache` — cross-file mutable flag** with 4 consumers and no owner | Medium | Low | `macos9_disk_cache.c:62`, http/tls fetchers, `window.c` |
| 4 | **Mouse drag tracking DUPLICATED** in `main.c:919` and `window.c:514` at depth 8-9 | Medium | Medium | `main.c`, `window.c` |
| 5 | **Mid-file `#include`s** at `macsurf_qjs.c:1901,9395` | Low | Low | `macsurf_qjs.c` |
| 6 | **`hctx_poll` is 634 lines** — the TLS fetcher's main state machine | Medium | High | `macos9_tls_fetcher.c:2726-3359` |
| 7 | **43% of `macsurf_timebase.c` is comments** — timebase subsystem is well-understood now | Low | Low | `macsurf_timebase.c` |
| 8 | **`js_stub.c` is forever-dead code** — `WITH_QUICKJS` always defined, guard makes entire file compile to nothing | Low | Trivial | `js_stub.c` |
| 9 | **`misc_stub.c` is a dumping ground** — 268 lines of stubs for 7 different subsystems | Low | Low | `misc_stub.c` |
| 10 | **`macos9_wheel.c` is 73% comments** — retired feature, entire file is historical record | Low | Trivial | `macos9_wheel.c` |

---

## 6. Prioritized Cleanup Roadmap

### Phase 1: Quick Wins (1-2 rounds, LOW risk)

These can be done immediately with no behavioral change:

1. ✅ **Remove `js_stub.c` from the build.** The `#ifndef WITH_QUICKJS` guard makes it compile to nothing. MacSurf always builds with QuickJS. Remove from `MacSurf.mcp` (user). Remove from git. **Risk: zero.** *(Done: Phase 1, `f453f3f1`)*

2. **Consolidate `macsurf_http_skip_next_cache` into a context struct.** Pass it through the fetch context rather than as a bare global extern across 4 files. Add accessors. **Risk: very low** (single-writer flag).

3. ✅ **Move mid-file `#include`s to file top** in `macsurf_qjs.c` (lines 1901, 9395). The includes at 1901 (`macos9_reconvert.h`) and 9395 (`content/content_factory.h`, `content/content_protected.h`) work where they are but signal incomplete refactoring. **Risk: low** (content_protected.h at line 45 is the early include; the duplicate at 9396 can just be removed). *(Done: Phase 1, `f453f3f1`)*

4. ✅ **Extract diagnostic functions** into `macsurf_qjs_audit.c`. 8 functions extracted: `emit_timer_profile`, `geom_stats`, `gc_note`, `wrap_stats`, `perf_totals`, `emit_js_profile`, `audit_reset`, `page_js_summary` (396 lines). ~30 counters made file-scope for cross-TU access. `macsurf_qjs.c`: 11,497 → 11,126 lines. *(Done: Phase 2, `b5e2c9f3` + fixes)*

5. ✅ **Remove `macos9_wheel.c` from the build** or reduce to a 5-line doc comment. The 73% comment density reflects a retired feature whose sole purpose is historical record. **Risk: zero** — no code path reaches it (`macos9_wheel_install()` is a documented no-op). *(Done: Phase 1, `f453f3f1`)*

### Phase 2: Structural Refactors (3-5 rounds, MEDIUM risk)

These change code structure without changing behavior:

6. ◐ **Extract shared fetcher cache layer.** Partially done — shared `macos9_find_line()` extracted (`be4cbc57`); shared request-header helpers done in prior cleanup (`533da123`/`9b59b481`). Cache accumulator + cache-hit replay remain (tightly coupled to context structs, needs shared struct refactor).

7. **Table-drive `register_browser_globals()`** in `macsurf_qjs.c`.
   - Define a `{name, type, getter, setter, flags}` registration table (~100 entries)
   - Replace the 1,417-line function with a loop over the table
   - **Risk: medium** — must preserve property descriptor flags and inheritance chain. Test with harness Test 45 (hackaday real bundle).

8. ◐ **Extract shared fetcher header parser.** Partially done — shared cookie + Sec-Fetch header helpers extracted (`533da123`/`9b59b481`); shared buffer constants (`854233ae`). Header parse loops still duplicated (different context structs).

9. ◐ **Flatten mouse/scroll handlers.** Partially done — shared `macos9_throttled_repaint()` extracted (`ebc502c2`), duplicate TickCount/updateEvt pattern removed from both StillDown() loops, nesting reduced by 2 levels. Full drag-tracker extraction + StillDown() utility remain.

### Phase 3: Deep Architecture (defer, HIGH risk/effort)

These are substantial refactors that change ownership boundaries:

10. **Split `macsurf_qjs.c` by subsystem.**
    - `macsurf_qjs_core.c` — runtime/heap/thread lifecycle, `js_exec`, timers
    - `macsurf_qjs_dom.c` — element wrappers, traversal, mutation, textContent/innerHTML
    - `macsurf_qjs_events.c` — event dispatch, listener management, inline handler binding
    - `macsurf_qjs_css.c` — computed style, matchMedia, geometry
    - `macsurf_qjs_globals.c` — browser globals registration (the table-driven version from #7)
    - **Risk: high** — 475 fix markers means 475 points of behavioral subtlety. Requires harness Test 38-46 full pass before shipping.

11. **Decompose `hctx_poll` in the TLS fetcher.**
    - 634 lines across 6 major phases (cache-hit, connect, TLS handshake, request-send, header-receive, body-receive)
    - Each phase is a state-machine case; extract to `hctx_poll_<phase>()` functions
    - **Risk: high** — this is the primary fetch path. Every page load goes through it.

12. **Context-struct-ify `window.c`'s 38 statics.**
    - 25 icon/GWorld cache variables, URL suggestion state, JS title lock
    - Bundle into a `struct macos9_window_resources` and pass explicitly
    - **Risk: medium** — window.c is the most-touched file in the frontend; mechanical but broad.

---

## 7. Legacy-Safe Patterns for the Refactor

When executing the roadmap, prefer these patterns that work under CW8 C89:

### Guard Clauses (Already Used, Apply More)

```c
/* BEFORE (deep nesting) */
if (gw != NULL) {
    if (gw->bw != NULL) {
        if (gw->bw->current_content != NULL) {
            /* 7 more levels... */
        }
    }
}

/* AFTER (guard clauses) */
if (gw == NULL) return;
if (gw->bw == NULL) return;
struct hlcache_handle *c = gw->bw->current_content;
if (c == NULL) return;
/* flat code follows */
```

### Context Structs (Instead of Global Externs)

```c
/* BEFORE: scattered globals */
extern int macsurf_http_skip_next_cache;  /* 4 files declare this */
extern OTClientContextPtr macos9_ot_context;

/* AFTER: bundled in a context */
struct macos9_fetch_env {
    OTClientContextPtr ot_ctx;
    int skip_next_cache;
    /* ... */
};
/* Single instance in main.c, passed to fetchers at init */
```

### Table-Driven Registration (Instead of God Functions)

```c
/* BEFORE: 1,417-line function */
static void register_browser_globals(JSContext *ctx) {
    JS_SetPropertyStr(ctx, nav, "userAgent", ...);  /* line 8043 */
    /* ... 1400 more lines ... */
}

/* AFTER: table-driven */
static const JSPropertyDef browser_globals[] = {
    {"navigator", JS_PROP_OBJECT, setup_navigator, NULL, 0},
    {"console",   JS_PROP_OBJECT, setup_console,   NULL, 0},
    /* ... */
    {NULL, 0, NULL, NULL, 0}
};
static void register_browser_globals(JSContext *ctx) {
    for (int i = 0; browser_globals[i].name; i++)
        register_property(ctx, &browser_globals[i]);
}
```

### Function Pointers for Strategy Pattern (State Machines)

```c
/* BEFORE: 634-line hctx_poll with embedded switch */
static void hctx_poll(struct macos9_https_ctx *c) {
    /* 634 lines of state-machine cases inline */
}

/* AFTER: phase dispatch table */
typedef void (*hctx_phase_fn)(struct macos9_https_ctx *);
static const hctx_phase_fn hctx_phases[] = {
    hctx_poll_cache_hit,
    hctx_poll_connect,
    hctx_poll_tls_handshake,
    hctx_poll_send_request,
    hctx_poll_recv_headers,
    hctx_poll_recv_body,
};
static void hctx_poll(struct macos9_https_ctx *c) {
    if (c->phase < HCTX_N_PHASES)
        hctx_phases[c->phase](c);
}
```

---

## 8. Verification Gates

Before considering any phase complete:

1. **Harness full pass** — `make -C harness check` (tests 1-46, including Test 45's real hackaday bundle and Test 46's DOM conformance sweep)
2. **`make -C harness check-macdefault`** — compile with the Mac's actual `MACSURF_JS_*` switch configuration (quiesced — JS_GEOMETRY, JS_VIEW_EVENTS, JS_FIRE_LOAD all OFF)
3. **git status clean** — no untracked `.c`/`.h` files (`git ls-files --others --exclude-standard '*.c' '*.h'`)
4. **Hardware smoke test** — launch on the 10.3 iMac, load a page, check `MacSurf Debug.log`

---

*Audit compiled 2026-08-05 from 4 parallel codebase traversals. Branch `cleanup` at `533da123`. All line numbers and function names verified against the live tree.*
