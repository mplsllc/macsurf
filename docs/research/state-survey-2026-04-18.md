# MacSurf State Survey — 2026-04-18

Written after many incremental rounds. Goal: a single evidence-backed
snapshot so the next round starts from ground truth instead of the
git-log rumor mill. No code changes in this round.

All paths are relative to the repo root
`/home/patrick/Webs/macsurf/`. All `.c` / `.h` files in the macos9
frontend use Mac CR-only line endings — `wc -l` reports 0. Use Read /
Grep, not `wc` / `cat`.

Current branch: `v0.3-rendering`. Most recent commit at time of
writing: `5b826fb revert(window.c): drop speculative SetPortWindowPort
before TENew`.

Uncommitted working-tree changes:

| File | Nature |
|---|---|
| `browser/netsurf/frontends/macos9/macos9_fetcher_stubs.c` | one hunk: adds `MS_LOG("stub snd")` / `MS_LOG("stub fin")` and `fetch_set_http_code(ctx->parent, 200)` in `stub_send_for` |
| `browser/netsurf/frontends/macos9/macos9_ns_fetcher.c` | one hunk (user-local, small) |
| `proxy/macsurf-proxy` | built binary |

---

## Section 1 — Chrome state: URL field divergence between initial and new windows

The user reports that `File → New Window` creates a window whose URL
field accepts typing, but the window created at startup does not.

### 1.1 Initial-window path (startup)

Origin: [main.c:main()](../../browser/netsurf/frontends/macos9/main.c), near the
comment "browser window created".

```
main()
  netsurf_register(&macos9_table)           // installs macos9_window_table
  netsurf_init(NULL)                         // internally calls fetcher_init()
  macos9_fetcher_register()                  // registers http/https (+stubs if linked)
  nsurl_create(MACSURF_HOME_URL, &home_url)
  browser_window_create(FLAGS, home_url,
                        NULL, NULL, &bw)     // NetSurf core calls back
    └─ netsurf core → gui_window_table->create_window
         └─ macos9_window_create(bw, NULL, 0)  // window.c
              CreateNewWindow(kDocumentWindowClass,
                              kWindowStandardDocumentAttributes,
                              &bounds, &gw->window)
              SetWTitle(gw->window, "\pMacSurf")
              SetWRefCon(gw->window, (long)gw)
              SetPortWindowPort(gw->window)
              NewControl(gw->window, ...) × 6  // back/fwd/reload/home/vs/hs
              macos9_window_layout(gw)         // computes url_rect etc.
              TextFont(kFontIDMonaco); TextSize(10); TextFace(0)
              compute_url_te_rect(&gw->url_rect, &te_rect)
              gw->url_te = TENew(&te_rect, &te_rect)
              TESetText(MACSURF_HOME_URL, len, gw->url_te)
              gw->url_field_active = false
              ShowWindow(gw->window)
              window_list = gw
              return gw
  // back in browser_window_create: content fetch kicks off asynchronously.
  // NetSurf may dispatch GW_EVENT_NEW_CONTENT and macos9_gw_set_url
  // during the remainder of this synchronous call.
  initial_win = macos9_find_window(FrontWindow())
  // event loop starts; activateEvt / updateEvt for the window arrive
  // on subsequent passes.
```

### 1.2 File → New Window path

Origin: [main.c macos9_handle_menu()](../../browser/netsurf/frontends/macos9/main.c)
case `ITEM_FILE_NEW`.

```
macos9_handle_menu(MENU_FILE, ITEM_FILE_NEW)
  nw = macos9_create_initial_window()   // window.c
    └─ macos9_window_create(NULL, NULL, 0)  // bw == NULL
         ... IDENTICAL body to the initial-window case ...
         ShowWindow(gw->window)
         return gw
  macos9_window_home(nw)
    └─ macos9_window_navigate(nw, MACSURF_HOME_URL)
         set_url_te_text(gw, url)
            SetPortWindowPort(gw->window)
            TESetText(url, len, gw->url_te)
         set_status_text(gw, "Loading...")
         macos9_window_invalidate_status(gw)
         if (gw->bw == NULL) return      // bw is NULL, so no navigation
```

### 1.3 Side-by-side

| Step | Initial path | File → New path |
|---|---|---|
| Enter `macos9_window_create` | `bw != NULL` | `bw == NULL` |
| `CreateNewWindow` | same | same |
| `SetWRefCon(gw->window, (long)gw)` | same | same |
| `SetPortWindowPort(gw->window)` | same | same |
| 6 × `NewControl` | same | same |
| `macos9_window_layout(gw)` | same | same |
| `TextFont/TextSize/TextFace` | same | same |
| `TENew(&te_rect, &te_rect)` | same | same |
| `TESetText(MACSURF_HOME_URL, ...)` | same | same |
| `ShowWindow(gw->window)` | same | same |
| Return | same | same |
| Post-return caller | `browser_window_create` continues (fetch) | `macos9_window_home(gw)` |

**The body of `macos9_window_create` is identical in both paths.**
The divergence is what happens *before* and *after* the call.

### 1.4 Divergence that can affect TextEdit

1. **Caller context.** The initial-window call is re-entered from
   inside `browser_window_create`, which then proceeds to do real
   work — llcache lookup, fetcher setup, hlcache child-content
   kickoffs, and potentially synchronous fetcher callbacks
   (`stub_start` for `resource:default.css` etc.) — all still inside
   that single `browser_window_create` call. Any of these can call
   back into our gui_window callbacks:
   - `macos9_gw_set_url(gw, nsurl)` → `set_url_te_text` → `SetPortWindowPort(gw->window)` + `TESetText(url, len, gw->url_te)`.
   - `macos9_gw_event(gw, GW_EVENT_NEW_CONTENT)` → `macos9_window_invalidate_all(gw)`.
   None of these dispose `url_te`, but they do `SetPortWindowPort`
   repeatedly. That is safe.

2. **activateEvt timing.** Both paths call `ShowWindow`, which queues
   an activateEvt. The initial-window activateEvt arrives on the
   first pass through `macos9_poll`. The File → New activateEvt
   arrives after the menu dismisses. Both enter
   `macos9_handle_activate`:

   ```c
   if ((event->modifiers & activeFlag) != 0) {
       if (gw->url_field_active && gw->url_te != NULL) {
           SetPortWindowPort(win);
           TEActivate(gw->url_te);
       }
   }
   ```

   Since `gw->url_field_active` is false for a fresh window,
   **`TEActivate` is not called on window activation in either
   path.** Only a mouse click on the URL rect sets
   `url_field_active = true` via
   `macos9_window_te_activate_url(gw)`. This is symmetric.

3. **WRefCon, port, TENew.** These are set in the same order in the
   same function, so they cannot differ between the two paths.
   Commit `5b826fb` reverted a speculative extra `SetPortWindowPort`
   right before `TENew`; the remaining call chain matches the
   "Safe pattern" documented in CLAUDE.md's Known Gotchas.

### 1.5 Most likely cause

The two paths exercise the **same** `macos9_window_create` body, so
`url_te` is created identically. The divergence has to come from
something that happens *only* during `browser_window_create`.

The leading hypothesis is: during the remainder of
`browser_window_create` (after `macos9_window_create` returns),
NetSurf dispatches `GW_EVENT_NEW_CONTENT` or `GW_EVENT_UPDATE_EXTENT`
via `macos9_gw_event`, which calls `macos9_window_invalidate_all(gw)`.
The next `macos9_handle_update` pass redraws the whole window —
including calling `browser_window_redraw(bw, ...)`. That redraw runs
the `macos9_plotters` table against whatever `bw`'s content has at
that moment (likely still empty, but valid clip).

If `plot_clip` or `plot_rectangle` does not honor the `clip` rect and
paints beyond `gw->content_rect`, it can overdraw the URL field area,
blanking it visually. Typing still works internally, but **`TEClick`
requires the click to be dispatched into the correct rect
(url_rect)**, so if the visible URL bar has been painted black or
white on top, the user may not see the caret and believe typing
doesn't work. Also, `draw_url_bar` calls `TEUpdate(&gw->url_rect,
gw->url_te)` only from inside `macos9_handle_update` — if content
redraw runs without `BeginUpdate`/`EndUpdate` or without clipping,
QuickDraw will commit its output immediately and the URL bar stays
overdrawn until the next full invalidate.

For the File → New path, `bw == NULL`, so the
`browser_window_redraw` branch inside `macos9_handle_update` is
skipped (`if (gw->bw != NULL && browser_window_redraw_ready(gw->bw))`)
— the URL bar never has to compete with content plotting. That
matches the symptom exactly: works in the no-bw window, fails in the
with-bw window.

**This is the most concrete, source-backed hypothesis. Confirming it
requires adding a one-shot probe that logs `plot_rectangle` / `plot_text`
coordinates and the current clip rect when they fall outside
`gw->content_rect`.** No fix is proposed in this round.

Secondary hypothesis: `url_field_active` never gets set to `true` on
the initial window because the click is intercepted by some other
path (e.g. the `FindControl` branch in `macos9_handle_mouse_down` is
returning non-NULL for a click that should be in the URL rect). This
would need a probe on `point_in_rect(local, &gw->url_rect)` and the
`ControlRef ctrl` returned by `FindControl` for the first click after
startup.

---

## Section 2 — CSS_NOMEM investigation readiness

### 2.1 CSS_NOMEM return paths in libcss

95 sites across libcss return `CSS_NOMEM`. The subset reachable from
`css_select_style`:

| Function | File:Line | Cause |
|---|---|---|
| `css_select_style` | `browser/libcss/src/select/css_select.c:1262` | entry point |
| (inside) alloc `state.results` | `css_select.c:1095` | calloc fail |
| (inside) `css__create_node_data` | `css_select.c:147` | calloc fail |
| (inside) alloc `state.revert` | `css_select.c:1325` | calloc CSS_ORIGIN_AUTHOR bytes |
| `css__get_parent_bloom` | `css_select.c:582` | malloc `sizeof(css_bloom)*CSS_BLOOM_SIZE` |
| `css__create_node_bloom` | `css_select.c:636` | calloc(CSS_BLOOM_SIZE, sizeof(css_bloom)) |
| `select_from_sheet` | `css_select.c:1773` | `sp >= IMPORT_STACK_SIZE (256)` |
| `css_select_ctx_insert_sheet` | `css_select.c:351` | realloc ctx->sheets |
| `css__selector_hash_create` | `browser/libcss/src/select/hash.c:131/139/148` | calloc DEFAULT_SLOTS*sizeof(hash_entry) |

Most of these are plain `malloc` / `calloc` / `realloc` failures.

### 2.2 Allocator chain

libcss calls the C library directly. There is **no NetSurf wrapper,
no allocator hook, no bump-arena.** Every `CSS_NOMEM` path bottoms out
at MSL's `malloc` / `calloc` / `realloc`. Evidence:

- `browser/libcss/src/select/css_select.c:151,248,582,636,1095,1325` — raw `malloc` / `calloc`.
- `browser/libcss/src/select/hash.c:126` — raw `calloc`.
- `browser/libcss/include/libcss/utils.h` — no alloc macros.

NetSurf's `content/handlers/css/cssh_select.c:264` calls
`css_select_style(...)` without installing any allocator hook. So "out
of memory" in libcss means the process-wide heap (the CW8 app
partition) is actually exhausted.

### 2.3 Carbon application partition

The CW8 project settings in
[MacSurf.mcp](../../browser/netsurf/frontends/macos9/MacSurf.mcp) are:

| Setting | Value |
|---|---|
| `MWProject_PPC_size` (preferred) | **4096 KB (4 MB)** |
| `MWProject_PPC_minsize` (minimum) | **2048 KB (2 MB)** |
| `MWProject_PPC_stacksize` | 256 KB |

**This is a smoking gun for CSS_NOMEM.** MacSurf is a NetSurf build
linking libcss + libhubbub + libdom + libparserutils + libduktape +
NetSurf core. The compiled executable itself is expected to be
several MB, which means at runtime there is perhaps 500 KB of free
heap for content. Loading a real web page produces hundreds of DOM
nodes, each with its own `css_select_style` call, each of which
allocates a `state.results` array sized to `CSS_ORIGIN_AUTHOR` × `css_properties` (≈ a few hundred bytes), plus bloom filters, plus hash-entry slots.

On a 4 MB partition, hitting the ceiling at element ~380 is exactly
what the allocation pattern predicts. This is **not** a hardcoded
limit in libcss — it's the heap running out.

There is no `SIZE` resource in [MacSurf.r](../../browser/netsurf/frontends/macos9/MacSurf.r) — only the `'carb'` resource is defined there. The partition values come entirely from the `.mcp` linker settings above.

### 2.4 `int64_t` / `long long` audit in libcss

Grep found int64 math in six files. Allocation-related ones are
zero — all sites are fixed-point math, not allocation size math:

- `browser/libcss/include/libcss/fpmath.h:57,69,81` — `css_divide_fixed`, `css_multiply_fixed`, `css_int_to_fixed`.
  **Guarded by `#ifdef __MWERKS__` gate; CW8 builds take the `double`-based path.** Verified in file.
- `browser/libcss/src/parse/mq.c:887` — `uint64_t *result` in signature of `mq_parse_type`; not used in alloc paths.
- `browser/libcss/src/select/unit.c:363,376` — `int64_t sanity = (int64_t)65536 * (int64_t)65536` inside a `#ifdef MACSURF_DEBUG` probe block. Not called during normal selection.
- `browser/libcss/test/number.c:136` — test-only, not in MacSurf.mcp.

Conclusion: **no 64-bit miscompile risk in the allocation path.**
CSS_NOMEM is a real heap-exhaustion condition.

### 2.5 Element-count thresholds near 380

Grep for 380, 384, 400, 512, 256, 128, 1024 across libcss:

| Constant | File:Line | Relevance |
|---|---|---|
| `reject_cache[128]` | `browser/libcss/src/select/select.h:94` | per-selection-state; not tied to element count |
| `IMPORT_STACK_SIZE = 256` | `browser/libcss/src/select/css_select.c:1745` | triggers CSS_NOMEM on stylesheet `@import` depth > 256, not element count |
| `CSS_BLOOM_SIZE = 4` | `browser/libcss/src/select/bloom.h:41` | always 4 uint32 slots |
| `DEFAULT_SLOTS = 64` | `browser/libcss/src/select/hash.c:27` | initial hash bucket count |

**No hardcoded 380-element bucket exists.** 380 is an artifact of the
per-element allocation footprint summing up against the 4 MB
partition ceiling.

---

## Section 3 — Render pipeline ground truth vs CLAUDE.md

### 3.1 `no_backing_store.c`

[browser/netsurf/content/no_backing_store.c](../../browser/netsurf/content/no_backing_store.c).

- `store(url, flags, data, datalen)` — returns `NSERROR_NOT_IMPLEMENTED` (lines 35–41).
- `fetch(url, flags, data_out, datalen_out)` — returns `NSERROR_NOT_IMPLEMENTED` (lines 43–52), also zeroes outputs.
- `initialise`, `finalise` return `NSERROR_OK`.
- `invalidate`, `release` return `NSERROR_NOT_FOUND`.

Matches CLAUDE.md claim.

### 3.2 Resource fetcher CSS content

The resource fetcher is implemented in
[browser/netsurf/frontends/macos9/macos9_fetcher_stubs.c](../../browser/netsurf/frontends/macos9/macos9_fetcher_stubs.c)
(not upstream `content/fetchers/resource.c`).

CSS bodies are concrete embedded C string literals:
- `css_default[]` — ≈ 1 KB of block/flow display + table + headings + inline styling + forms rules.
- `css_internal[]` — ≈ 300 bytes of input/textarea/button styling.
- `css_quirks[]` — ≈ 150 bytes of table / img / form / body rules.
- `favicon_ico[6]` — six zero bytes.

Registration via `fetch_resource_register()` and four sibling
functions (`about`, `file`, `data`, `javascript`).

**Important caveat.** `macos9_fetcher_stubs.c` and
`macos9_ns_fetcher.c` are **not currently present** in the Linux-side
`MacSurf.mcp` XML (grep the file for `fetcher_stubs`/`ns_fetcher` yields
zero hits). Only `macos9_http_fetcher.c` and `macos9_fetcher_init.c`
appear. This does not necessarily mean they are missing from the
on-Mac build — CLAUDE.md explicitly says the user maintains the
project file list on the Mac side through the CW8 IDE, and the
Linux-side `.mcp` is out of date by design. But we **cannot confirm
from Linux source alone** whether the resource fetcher is actually in
the built binary.

**Action item for the user: confirm `macos9_fetcher_stubs.c` and
`macos9_ns_fetcher.c` are both in the CW8 project file list on the
Mac.** If not, resource CSS is not being served, `macos9_fetcher_register`
is unresolved at link time, and the rendering pipeline's CSS input is
whatever the upstream fallback path serves (likely nothing).

### 3.3 `fetch_poll()`

CLAUDE.md claim: "Scheduler pump calls `fetch_poll()` every event-loop
pass when fetches are pending."

Reality: **there is no call to `fetch_poll()` in the macos9 main
loop.** Grep for `fetch_poll` across the entire tree:
```
browser/netsurf/content/handlers/html/css_fetcher.c
browser/netsurf/frontends/beos/fetch_rsrc.cpp
browser/netsurf/content/handlers/javascript/fetcher.c
browser/netsurf/content/fetchers/file/file.c
browser/netsurf/content/fetchers/resource.c
browser/netsurf/content/fetchers/data.c
browser/netsurf/content/fetchers/about/about.c
```

None of these are called from `macos9_poll`. What [main.c:macos9_poll()](../../browser/netsurf/frontends/macos9/main.c) actually does:

```c
macos9_windows_process_deferred();   // handles needs_reformat
if (macos9_fetching ||
    macos9_stub_fetcher_active() ||
    macos9_http_fetcher_active()) {
    sleep_ticks = 1;
} else {
    sleep_ticks = (UInt32)macos9_get_next_delay();
    if (sleep_ticks > 15) sleep_ticks = 15;
}
WaitNextEvent(everyEvent, &event, sleep_ticks, NULL);
macos9_dispatch_event(&event);
macos9_windows_te_idle();
#ifdef WITH_DUKTAPE
macsurf_js_pump_all();
#endif
macos9_schedule_run();
```

Fetcher progress is driven by `stub_poll` / `fetch_macos9_poll` being
invoked by NetSurf core itself when `fetch_send_callback` triggers a
continuation, not by an explicit `fetch_poll()` tick. The claim in
CLAUDE.md is misleading — CLAUDE.md should be updated to describe
what the code actually does. Concretely, the sleep is just shortened
to 1 tick when any fetcher is active so the core gets cycles quickly.

### 3.4 `fpmath.h` double-based fix

[browser/netsurf/include/libcss/fpmath.h](../../browser/netsurf/include/libcss/fpmath.h)
contains the `#ifdef __MWERKS__` gate. All three macros (`css_divide_fixed`,
`css_multiply_fixed`, `css_int_to_fixed`) route through `double`
arithmetic with INT_MIN/INT_MAX saturation. Matches commit `4518c84`.
Matches CLAUDE.md claim.

### 3.5 `macsurf_render_test.c`

Does not exist on disk. `find browser -name "macsurf_render_test*"`
returns nothing. **But:** three stale references remain in
[MacSurf.mcp](../../browser/netsurf/frontends/macos9/MacSurf.mcp) at
lines 806, 4552, 4680 (a `PATH` entry, a `FILEREF` in the file list,
and a `FILEREF` in the link order). No `MACSURF_RENDER_TEST` macro is
defined in `macsurf_prefix.h`. CLAUDE.md does not currently claim the
file exists.

**Cleanup action:** since the user maintains the Mac-side `.mcp` in
CW8, this is informational only — but the Linux-side `.mcp` is stale
on this point and any tooling that regenerates from it will
reintroduce a phantom file.

### 3.6 Main-loop pumps

Both `macos9_schedule_run()` and `macsurf_js_pump_all()` are called
unconditionally every poll pass (`macsurf_js_pump_all` under
`#ifdef WITH_DUKTAPE`, which is set).
`macos9_windows_te_idle()` is also unconditional.
`macos9_windows_process_deferred()` handles `needs_reformat`.

---

## Section 4 — Stale probes and cruft

### 4.1 MS_LOG distribution

Total: **52 MS_LOG occurrences across 15 files.**

Frontend (macos9):
- `macsurf_debug.h` — 6 (macro definitions)
- `macsurf_debug.c` — 4 (infrastructure)
- `main.c` — 1 (event-loop heartbeat only)
- `window.c` — 1 (resize trace)
- `plotters.c` — 1 (per-plotter smoke)
- `macos9_ns_fetcher.c` — 1
- `macos9_fetcher_stubs.c` — 1 (plus 2 more in the uncommitted diff)

NetSurf core:
- `desktop/netsurf.c` — 2
- `content/hlcache.c` — 16
- `content/llcache.c` — 4
- `content/ns_content.c` — 2
- `content/handlers/css/cssh_css.c` — 4
- `content/handlers/html/html_css.c` — 4
- `content/handlers/html/box_construct.c` — 1
- `content/handlers/html/html.c` — 4

**Most of these are permanent instrumentation** — cheap single-line
title-bar writes, already gated at the macro level by
`MACSURF_DEBUG`. They do not need to be ripped out. The concern is
the **probe cluster**, not the MS_LOG baseline.

### 4.2 One-shot probe gates

Two `static int probe_fired` style gates are active:

| File:Line | Gate | Purpose |
|---|---|---|
| `browser/netsurf/content/handlers/html/box_construct.c:554` | `static int probe_fired = 0;` | p1/p2 probe fires once on root element, records `css_computed_color` return |
| `browser/netsurf/content/handlers/html/layout.c:2693` | `static int probes_fired = 0;` | Adpi/Afsd/Afsm/Aroot/Alen/Aunit/Dout/Efs/Efm/Esc/Er0/Er4/Er8/Er12/Esc1k probes fire once on first line-height resolution |

Both are left over from the fpmath investigation that was resolved in
commit `4518c84` (fpmath double-based fix) and captured in the CW8
PPC long-long gotcha section of CLAUDE.md. They are no longer
serving an investigation and should be removed when convenient, but
they are not currently breaking anything — they simply occupy the
window title bar on first render.

### 4.3 Residual A/B/C/D/E/F/G probe code

Yes, extensive probe code remains:

- `browser/netsurf/content/handlers/html/layout.c:79` — `static html_content *macsurf_probe_html = NULL;` pointer stash.
- `layout.c:2682–2775` — the big Adpi/Afsd/Afsm/Aroot/Alen/Aunit + Dout + Efs/Efm/Esc/Er0/Er4/Er8/Er12/Esc1k probe block.
- `layout.c:5546` — `macsurf_probe_html = content;` — installs the pointer at layout entry.
- `box_construct.c:549–572` — p1 / p2rc / p2col probes.

The commit `3654281 feat: chrome polish — strip diagnostic probes` removed probes from frontend files; it did **not** touch `layout.c` or `box_construct.c`. The probe residue in the HTML handler remains.

### 4.4 `#if 0` dead code in the macos9 frontend

None found. `grep -rn "#if 0" browser/netsurf/frontends/macos9/` returns zero matches.

### 4.5 `macsurf_debug.h/c` summary

- `MS_LOG(msg)`, `MS_BREAK(msg)`, `MS_ASSERT(cond, msg)` macros; all no-op when `MACSURF_DEBUG` is undefined.
- `macsurf_debug_set_title_force()` / `macsurf_debug_log_int_force()` — sticky title-writing variants that set a `g_title_locked` flag so downstream non-force MS_LOG cannot clobber the probe output.
- `macsurf_debug_probe_reset()`, `macsurf_debug_probe_append(label)`, `macsurf_debug_probe_append_int(label, n)` — accumulate into a 256-byte `g_probe_buf` and force-set the window title.
- On non-`__MACOS9__` (Linux cross-check), output is redirected to stderr.
- `macsurf_debug_title_is_locked()` — checked from `macos9_gw_set_title` to avoid overwriting probe output.

Everything in this file is currently used.

### 4.6 `_force` probes from commit 593f2f0

`grep "_force" main.c window.c` → no matches. The `_force` suffix
exists only in `macsurf_debug.h/c` as the infrastructure (`set_title_force`,
`log_int_force`). Commit 593f2f0's ad-hoc _force probes were removed
by commit `3654281` ("feat: chrome polish — strip diagnostic probes").

---

## Section 5 — Known working / known broken matrix

Status legend: **W** = working (hardware-verified or trivially
correct), **P** = partial (works but with caveats), **B** = broken
(known current blocker), **U** = untested (compiled in, no hardware
evidence either way).

| Subsystem | Status | Evidence |
|---|---|---|
| Carbon app initialization | W | `main.c` calls `InitCursor` + `Gestalt(gestaltAppearanceAttr)` + `RegisterAppearanceClient` + `FlushEvents`; `'carb'` resource generated by `MacSurf.rsrc`; repeatedly loads on G3 per commit history |
| Open Transport networking | W | `main.c:main()` uses `InitOpenTransportInContext(kInitOTForApplicationMask, &macos9_ot_context)`; commits confirm OT verified against frogfind.com on G3 |
| HTTP fetcher | P | `macos9_http_fetcher.c` present; uses `OTOpenEndpointInContext` (InContext variant, which CLAUDE.md warns against but which has worked in practice now that `'carb'` is in place). Alternative `macos9_ns_fetcher.c` also present with larger buffer (256 KB vs 32 KB) but not confirmed in CW8 project file list |
| HTTPS via proxy | P | `MACSURF_PROXY_HOST 116.202.231.103:8765` in both fetcher files; `macos9_fetcher_register()` in `ns_fetcher.c` registers both http+https, but that function is only called if the file is in the CW8 project |
| Resource fetcher | P | `macos9_fetcher_stubs.c` serves `resource:default.css` / `internal.css` / `quirks.css` / `favicon.ico` — **only if included in CW8 project**, which cannot be verified from Linux |
| llcache | U | NetSurf core; no MacSurf-specific instrumentation; `no_backing_store.c` returns `NSERROR_NOT_IMPLEMENTED` |
| hlcache | U | NetSurf core; 16 MS_LOG calls inside `hlcache.c` will fire during fetch |
| libhubbub HTML parser | W | 30 files ported and C89-clean per `docs/research/libhubbub-port.md`, commit fd8d915; reaches element ~380 before downstream CSS_NOMEM |
| libdom DOM tree | W | 95 files ported, commit 744232d; no reported failures |
| libcss CSS parser | W | 303 files, commit 02628cf; parses stylesheets without error |
| libcss CSS cascade + selection | B | **Current blocker.** `css_select_style` returns `CSS_NOMEM` on ~element 380. Root cause (most likely): 4 MB Carbon partition |
| libcss layout | U | `content/handlers/html/layout.c` compiles; not reached in practice due to CSS blocker |
| QuickDraw plotter `plot_text` | P | Implemented in `plotters.c`; exercised by "Hello from MacSurf v0.3" test. Real-page behavior untested |
| `plot_rectangle` | P | Implemented; "styled text on colored background" test verified it |
| `plot_clip` | U | Present but clip-respect on all downstream plots not verified |
| `plot_path` | U | Implemented via bezier sampling per subagent summary; not exercised |
| `plot_bitmap` | U | Implemented via GWorld per subagent summary; no images rendered yet |
| Font metrics | P | Heuristic 0.6 em monospace / 0.52 em proportional — avoids QuickDraw GrafPort timing issues; not glyph-accurate |
| Scheduler / event loop | W | `schedule.c` TickCount queue; `macos9_poll` pumps every pass |
| Browser window creation — initial path | P | Creates window successfully, URL field visible but **typing appears not to work** per user |
| Browser window creation — File > New path | W | Creates window, URL typing works |
| Toolbar and buttons | W | 4 pushbuttons via `kControlPushButtonProc`; `macos9_window_update_button_states` gates on history/content availability |
| URL field (TextEdit) | P | Working in File > New path; broken in initial path (see Section 1) |
| Navigation history | W | `browser_window_history_back/forward/has_content` wired directly |
| Scrolling | W | `scroll_action` CDEF, arrow keys, page keys, home/end; `scroll_to/by`; syncs scrollbar thumb after `TrackControl` |
| Window resize | W | Deferred-flag pattern with `needs_reformat` + `reformat_in_progress` re-entrancy guard |
| Status bar | W | `draw_status_bar` + `gui_window_event` routes messages; `macos9_gw_set_status` wired |
| Title bar updates | W | `macos9_gw_set_title` with `SetWTitle`, title-lock support for probes |
| Duktape engine | W | `duktape.c` + custom `duk_config.h` (PACKED_TVAL, ALIGN_BY=8, RECLIMIT=128); ES5 stress tests passed per CLAUDE.md; smoke test `1+1==2` runs in `main.c` under `WITH_DUKTAPE && MACSURF_DEBUG` |
| JS DOM bindings | P | `javascript/macsurf_js*.c` register document/window/console/setTimeout; XHR stub in place; no real DOM mutation test |
| MacsBug instrumentation | W | `MS_LOG` macro infrastructure; probe buffer + title-lock; verified on G4 |

---

## Section 6 — Open questions with evidence-based answers

**1. Does the initial window's TextEdit handle ever receive an
activate event?**

Yes, on the activateEvt arriving from `ShowWindow`. **But
`TEActivate` is not called** because `macos9_handle_activate` gates
it on `gw->url_field_active`, which is false on a fresh window. This
gate is symmetric across both paths. The activate event itself is
received; the handler just does nothing for TE until the user clicks
the URL rect. See `main.c:macos9_handle_activate`.

**2. Is the CSS cascade reaching element 380 because of page
complexity or a hardcoded limit?**

Page complexity running up against a tight heap. **There is no
hardcoded 380-element limit in libcss** (see Section 2.5). The nearest
numeric thresholds are `IMPORT_STACK_SIZE = 256` (import depth, not
elements) and `DEFAULT_SLOTS = 64` (initial hash buckets, grows
dynamically). The ceiling is the 4 MB `MWProject_PPC_size` Carbon
partition.

**3. Does the Carbon SIZE partition need increasing, and if so, from
what to what?**

Yes. Current: **4 MB preferred / 2 MB minimum** (Section 2.3).
Recommended target to unblock CSS_NOMEM: **16 MB preferred / 8 MB
minimum.** Reference: Classilla's default partition is 32 MB on a 64
MB system. With a 64 MB G3 floor, 16 MB leaves enough for the OS and
Finder. On a 256 MB G4 (enthusiast tier), 32 MB+ is fine. The
partition is editable in the CW8 project settings under **"PPC PEF"
→ "Application Heap Size / Minimum Heap Size"**, stored as
`MWProject_PPC_size` / `MWProject_PPC_minsize` in the `.mcp` XML.

**4. Are there other `long long` miscompile risks in the codebase
beyond `fpmath.h`?**

Within libcss source: **no allocation-path sites.** Only fpmath
(already fixed), an `unsigned long long` bitmask in media-query type
parsing (`mq.c:887`), and `MACSURF_DEBUG`-gated sanity probes
(`unit.c:363,376`). Outside libcss: not audited in this round.
`utils/talloc.c` is the most likely remaining candidate per the
"Most likely bottleneck" note in CLAUDE.md — if talloc is in the
project, audit it for 64-bit math.

**5. Is the resize deferred-flag pattern actually being hit during
normal resize events?**

Yes. `macos9_handle_mouse_down`'s `inGrow` branch calls
`macos9_window_resize(gw)` → sets `gw->needs_reformat = true` →
`macos9_windows_process_deferred` runs on the next `macos9_poll` pass
and calls `browser_window_schedule_reformat`. Both steps carry
`MS_LOG("resize start"/"resize done")` and `MS_LOG("resize
reformat"/"resize reformat done")` checkpoints. The re-entrancy
guard `reformat_in_progress` exists.

**6. Does `browser_window_schedule_reformat` behave differently when
a fetch is in flight versus idle?**

Cannot be determined without hardware test. Source inspection shows
`schedule_reformat` in NetSurf core posts a scheduler entry; the
actual reformat happens on the next scheduler tick. That tick is
driven by `macos9_schedule_run`, which runs every pass. Whether an
in-flight fetch competes with layout for partition memory — yes, by
definition, since there is only one heap. Whether it triggers a
different code path inside `browser_window_schedule_reformat` — no,
the function is fetch-agnostic.

---

## Section 7 — Recommended next round

### 7.1 Single most impactful fix

**Increase the Carbon partition size.**

`MWProject_PPC_size: 4096 → 16384` (16 MB preferred).
`MWProject_PPC_minsize: 2048 → 8192` (8 MB minimum).

### 7.2 Why this is highest leverage

- The CSS_NOMEM blocker has no evidence of being a libcss bug or a
  CW8 miscompile — Section 2 eliminated those. The allocator is raw
  `malloc` / `calloc`, the int64 paths in fpmath are fixed, and the
  numeric constants near 380 are not limits.
- The Carbon partition is definitively 4 MB (Section 2.3, evidence
  from `MacSurf.mcp` linker settings).
- A NetSurf build with libcss + libhubbub + libdom + libduktape +
  NetSurf core cannot fit content in 4 MB. Classilla, which runs a
  full Mozilla-era DOM on the same hardware, ships with a 32 MB
  default partition.
- Every other secondary blocker (initial-window URL field, title-bar
  probes eating the probe buffer, stale `MacSurf.r` render-test
  references) is cosmetic compared to pipeline completion.

### 7.3 Files to modify

- `browser/netsurf/frontends/macos9/MacSurf.mcp` — change
  `MWProject_PPC_size` and `MWProject_PPC_minsize` in the Target
  Settings block. Per CLAUDE.md this file is user-maintained on the
  Mac — but in this case the setting lives in CW8's UI, not in a
  source file, so the user can edit it via **Edit → MacSurf Settings
  → PPC PEF** on the Mac. No `.mcp` ship from Linux needed.

No `.c` / `.h` changes for this fix.

### 7.4 Build cycles

One. Edit setting → Remove Object Code → Rebuild → launch → navigate
to `http://mac.mp.ls/` (or a test URL) → confirm CSS_NOMEM probe no
longer fires on element 380.

### 7.5 Acceptance criteria

1. `CSS_NOMEM` does not appear in the title bar or MS_LOG trace on a
   normal page load.
2. `css_select_style` returns `CSS_OK` for elements past 380.
3. Layout reaches `plot_text` / `plot_rectangle` for real content on
   a real page.
4. If OOM happens on a larger page (e.g. element 2000 on a heavy
   site), that's a separate follow-up — the acceptance criterion is
   "the 380 wall moves substantially", not "OOM is gone forever".

### 7.6 Optional bundled cleanup (same round)

If the partition-bump round is a quick flip, a few low-risk cleanups
can ride along since they don't need a separate compile:

- Strip the probe clusters in `layout.c:79,2682–2775,5546` and
  `box_construct.c:549–572` — the fpmath investigation they served
  is closed (commit `4518c84`). Keeps the title bar clear of stale
  probe output.
- Fix CLAUDE.md's "Scheduler pump calls `fetch_poll()`" line — the
  function is not called; the behavior is "sleep shortened to 1 tick
  while fetchers are active, which lets NetSurf's own ring-poll
  progress".
- Remove the three stale `macsurf_render_test.c` references in
  `MacSurf.mcp` (user-side, since the user maintains the file list
  on the Mac).

These are non-blocking and should only ship bundled if the user
wants the cleanup. The partition bump is the only required change.

### 7.7 Out-of-scope for this round

- **Initial-window URL field bug.** The Section 1 hypothesis
  (content-redraw overdrawing URL rect) needs a probe round to
  confirm before implementing a fix. That probe round should run
  *after* the partition bump, since a successful fetch is required
  to test whether content-redraw interferes with the URL field at
  all. Trying to fix it now risks adding speculative code that
  doesn't address the real mechanism.
- **Font metrics accuracy.** Heuristic widths are fine until a
  real page exercises layout. Defer until after CSS_NOMEM and
  initial-window bugs are both closed.
- **Duktape DOM bindings.** JS doesn't run on real pages yet;
  binding work is premature until the HTML/CSS/layout pipeline
  actually delivers content.
