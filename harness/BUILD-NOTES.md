# S0 — Linux + ASan reconvert-UAF harness (build notes)

Goal: reproduce the reconvert `dom_string` use-after-free (the M5/S2 blocker) deterministically on Linux under AddressSanitizer, in ms, with clean free/use stacks — so we fix it in a sandbox before re-enabling reconvert on the Mac (the feature that crash-looped the browser, gated off since fixes489). Linux-only dev tool; NEVER shipped in a fix tar, NOT in MacSurf.mcp.

## Layer 1 — QuickJS (DONE, builds under ASan)

Use the COMPLETE `quickjs-macos9/` tree (not `browser/libquickjs/`, which is missing the generated headers `quickjs-c-atomics.h` / `builtin-array-fromasync.h`). The QuickJS *version* doesn't matter for this harness — the UAF is in libdom/box_construct, not QuickJS internals; QuickJS only drives the DOM mutation. (If a macsurf_qjs.c API mismatch appears against quickjs-macos9/quickjs.h, reconcile then — both report JS_EVAL_OPTIONS_VERSION 1, so they're close.)

Working compile line (per file: quickjs, libregexp, libunicode, dtoa), from `quickjs-macos9/`:
```
gcc -c -O1 -g -fsanitize=address -pthread -D_GNU_SOURCE -include pthread.h \
    -DCONFIG_VERSION='"harness"' -I. <file>.c -o <file>.o
```
Key gotchas solved:
- Do NOT define `MAC_OS_9` on Linux — it pulls in Toolbox types (`UnsignedWide` from `Microseconds()`).
- `cutils.h` uses `PTHREAD_CREATE_DETACHED` without including `<pthread.h>` on Linux → `-include pthread.h`.
- quickjs-ng needs generated headers (atomics, builtin-array-fromasync, builtin-iterator-zip*) — all present in `quickjs-macos9/`.

## Layer 2 — NetSurf libs (NEXT)

libwapcaplet, libparserutils, libhubbub, libdom, libcss — all have Makefiles (upstream NetSurf, build on Linux). Build them (or their .c sets) under ASan. These are platform-neutral C89.

## Layer 3 — NetSurf core html/box/content path (the big one)

`html_reconvert_content` pulls in the html content handler (html.c, box_construct.c, layout*, content_factory, hlcache, the css handler). This is most of NetSurf core. Two options:
- (A) hand-pick the .c files + -I paths + a Makefile.config, compile under ASan. Risk: large dependency graph, many link stubs.
- (B) reuse `browser/netsurf/frontends/monkey` — it already builds headless NetSurf core on Linux via the NetSurf Makefile; swap/add our QuickJS binding. The core-on-Linux 90% is done for us; delta is the JS binding.
Decision pending: try (A) minimal first; fall back to (B) if the graph is unwieldy.

## Layer 4 — macos9 glue + stubs

Compile `javascript/macsurf_qjs.c` + `macos9_reconvert.c` WITHOUT defining `__MACOS9__` (the 12 `#ifdef __MACOS9__` blocks — incl. the WaitNextEvent call — compile out). Provide Linux stub headers for the 3 Mac headers they include:
- `macos9.h` — fake: type decls + extern decls the glue uses (html_reconvert_content, browser_window_get_content, macos9_window_list_head, macos9_gw_bw, gui_window/browser_window accessors). NO Carbon.
- `macsurf_debug.h` — MS_LOG / macsurf_debug_log_writef → printf or no-op.
- `macsurf_timebase.h` — macsurf_qjs_get_now → clock_gettime.

## Layer 5 — driver (~200 lines) + repro

parse an HTML buffer with a CDATA/`<script>`/comment text node → run a JS snippet via macsurf_qjs.c that drops that node's dom_string refcount (removeChild / attribute rewrite / textContent=) → `macsurf_js_set_reconvert_enabled(1)` + call `html_reconvert_content(c)` directly (bypass debounce) → ASan traps the stale dom_string read (box_construct.c:1834/1844; tripwire at :1851).

FOLLOW-UP: identify the exact JS→DOM mutation entry point that drops the CDATA refcount (macsurf_dom_dispatch.c is an empty 187-line scaffold; the real mutation methods are in macsurf_qjs.c element methods — appendChild:1242 / removeChild:1261 / textContent set:1157 / setAttribute:1103).

## Status
- Layer 1 (QuickJS): building under ASan. ✓
- Layers 2–5: next-session build integration.
