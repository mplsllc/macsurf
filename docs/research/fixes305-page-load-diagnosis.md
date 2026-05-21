# fixes305, page-load diagnosis (markdown only)

Round purpose: diagnostic. Reconcile docs gap between CLAUDE.md and the
fixes260-304 sprint handoff, verify the file-backed log channel is
wired, inventory the MS_LOG checkpoints actually present in source,
and lock a decision tree that maps the last log line in
`MacSurf Debug.log` to a fixes306 scope.

No code shipped this round.

Last shipped (per git): **fixes304**, wires `macos9_schedule_run()`
into `macos9_poll()`. Untested by the user. fixes305 explicitly does
**not** assume fixes304 was the right fix.

---

## 1. CLAUDE.md vs. handoff, reconciliation

CLAUDE.md "Last shipped fix" line says **fixes266**. Git says **fixes304**.
Documentation gap is 38 fixes wide. Edits queued for fixes306+ (after
the page-load loop is closed); fixes305 does not touch CLAUDE.md so
that, if the load failure exposes a regression, we don't have to
revert documentation.

Concrete deltas the handoff records that CLAUDE.md does not yet reflect:

| Topic | CLAUDE.md state | Actual state |
|---|---|---|
| Last shipped fix | fixes266 | fixes304 |
| `'carb'` resource source | "binary `MacSurf.rsrc`" | Rez sources `MacSurf.r` + `Types.r`. Binary `.rsrc` silently fails through CW8 File Mappings (extension routes to Rez compiler). Fixed fixes297. |
| Open Transport entry point | "plain `InitOpenTransport()`" | `InitOpenTransportInContext` with manual `#define kInitOTForApplicationMask 0x00000002`. Plain variant is **not exported** by this CarbonLib build. Fixed fixes289-291. |
| MSL Carbon stdio | (not documented) | `MSL_All_Carbon.Lib` provides `InstallConsole` / `RemoveConsole` / `Read/WriteCharsToConsole` / `strdup` / `strcasecmp` / `strncasecmp` / `mkdir` / `stat`. Stubbing them silently corrupts FILE* table init and crashes before `main()`. Discovered fixes298, root cause of the "main never runs" mystery in fixes294-296. |
| Schedule runner | (not documented as mandatory) | `macos9_schedule_run()` MUST be called every event-loop tick or fetcher_poll self-schedules but never drains. Wired in fixes304. |
| libcss `_ALIGNED` cascade | (not documented) | `_ALIGNED` was used as a struct-close tag in `stylesheet.h` but never defined as a macro; CW8 read it as a global → multi-link conflict. Defensive `#define _ALIGNED` empty in 20+ headers. Fixed fixes271-277. |
| Project file list | "470 .c files / 443 in MacSurf.mcp" | Materially churned in fixes278-286, most `*_stub.c` and `dt_*.c` removed, real ports added. Authoritative list lives in `MacSurf.mcp` on the Mac side, not in CLAUDE.md. |

These are the edits that fixes306+ should apply once the page-load
loop is closed. **Not applied this round.**

## 2. File-backed log channel, verification

CLAUDE.md's Regression Audit Checklist requires three things for any
shipped subsystem: init wired, smoke confirmed, dependency documented.
Verifying the log channel:

| Requirement | Status |
|---|---|
| `macsurf_debug_log_init` symbol exists | ✓ `macsurf_debug_log.c` |
| Init call wired into `main()` | ✓ [main.c:191](../../browser/netsurf/frontends/macos9/main.c#L191), first executable line of `main()` |
| `MS_LOG` macro routes to log file | ✓ [macsurf_debug.c:190](../../browser/netsurf/frontends/macos9/macsurf_debug.c#L190) calls `macsurf_debug_log_write` |
| Documented in CLAUDE.md | ✓ "Regression Audit Checklist" + "File-Backed Diagnostic Channel" sections |

**Conclusion: log channel is wired. No fixes305a hotfix needed.** If
the user launches the build and `MacSurf Debug.log` does **not**
appear on the Desktop, three failure modes (in order):

1. Crash before [main.c:191](../../browser/netsurf/frontends/macos9/main.c#L191), i.e. CFM startup or MSL init aborts. This was the shape of the fixes294-298 "main never runs" mystery (MSL Console shadow). If it returns, suspect newly added stubs that shadow MSL.
2. `FindFolder(kOnSystemDisk, kDesktopFolderType, ...)` failing, log init goes silently inert per the channel's design.
3. `FSpCreate` failing, full / locked / read-only volume.

## 3. MS_LOG checkpoint inventory (verified by `git grep`)

This is what currently writes to `MacSurf Debug.log`. Anything not
listed here cannot appear in the log. Use this as the dictionary when
reading the log file in §5.

### Startup (main.c)

| Line | Message | Meaning |
|---|---|---|
| [main.c:192](../../browser/netsurf/frontends/macos9/main.c#L192) | `== MacSurf start ==` | First line in `main()`, after log init |
| [main.c:199](../../browser/netsurf/frontends/macos9/main.c#L199) | `InitOT FAIL` | `InitOpenTransportInContext` non-zero return |
| [main.c:202](../../browser/netsurf/frontends/macos9/main.c#L202) | `InitOT OK` | OT ready |
| [main.c:205](../../browser/netsurf/frontends/macos9/main.c#L205) | `Appearance OK` | `RegisterAppearanceClient` returned |
| [main.c:222](../../browser/netsurf/frontends/macos9/main.c#L222) | `netsurf_register done` | gui table registered |
| [main.c:224](../../browser/netsurf/frontends/macos9/main.c#L224) | `nsoption_init done` | options system live |
| [main.c:226](../../browser/netsurf/frontends/macos9/main.c#L226) | `netsurf_init done` | core init complete |
| [main.c:230](../../browser/netsurf/frontends/macos9/main.c#L230) | `http_fetcher registered` | http/https scheme handler installed |
| [main.c:233](../../browser/netsurf/frontends/macos9/main.c#L233) | `initial window created` | first window up |
| [main.c:235](../../browser/netsurf/frontends/macos9/main.c#L235) | `event loop exited` | main loop returned (clean Quit) |

### URL submit + navigate (window.c)

| Line | Message | Meaning |
|---|---|---|
| [window.c:88](../../browser/netsurf/frontends/macos9/window.c#L88) | `URL submit fired` | Return key in URL field captured |
| [window.c:89](../../browser/netsurf/frontends/macos9/window.c#L89) | `submit: no g or url_te` | TextEdit handle missing |
| [window.c:90](../../browser/netsurf/frontends/macos9/window.c#L90) | `submit: TEGetText null` | TextEdit returned no buffer |
| [window.c:91](../../browser/netsurf/frontends/macos9/window.c#L91) | `submit: empty` | TextEdit buffer was empty |
| [window.c:72](../../browser/netsurf/frontends/macos9/window.c#L72) | `navigate:` | navigate path entered |
| [window.c:73](../../browser/netsurf/frontends/macos9/window.c#L73) | `<url string>` | URL being navigated |
| [window.c:74](../../browser/netsurf/frontends/macos9/window.c#L74) | `nav: no g or empty u` | bailout: missing window or URL |
| [window.c:75](../../browser/netsurf/frontends/macos9/window.c#L75) | `nav: no bw` | bailout: window has no browser_window |
| [window.c:79](../../browser/netsurf/frontends/macos9/window.c#L79) | `nav: nsurl_create FAIL` | URL parsing failed |
| [window.c:80](../../browser/netsurf/frontends/macos9/window.c#L80) | `nav: calling browser_window_navigate` | about to enter NetSurf |
| [window.c:83](../../browser/netsurf/frontends/macos9/window.c#L83) | `nav: done` | `browser_window_navigate` returned |

### Fetcher (macos9_http_fetcher.c)

| Line | Message | Meaning |
|---|---|---|
| [macos9_http_fetcher.c:141](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L141) | `http_setup called` | `fetcher_operation_table::setup` invoked |
| [macos9_http_fetcher.c:55](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L55) | `mfs_open called` | `mfs_open` (post-setup transition) |

### Plotter (plotters.c)

| Line | Message | Meaning |
|---|---|---|
| [plotters.c:200](../../browser/netsurf/frontends/macos9/plotters.c#L200) | `plot_clip in=... content=... effective=...` | clip rect at draw time. Useful for the URL-field-overdraw hypothesis but noisy on real renders. |

### Dead-code MS_LOGs (will not appear)

These exist but are not on the live path:

- `macos9_ns_fetcher.c` (`http start`, `http finished`), file is partially dead. Its `macos9_http_fetcher_register` is never called from main.c (main.c calls `macos9_http_fetcher.c::macos9_http_fetcher_register` instead). `macos9_http_fetcher_active()` is the only function from `macos9_ns_fetcher.c` that's externally used.
- `macos9_fetcher_stubs.c` (`stub setup`, `stub start`), stub fetcher. If you see these, something is registering the stub for http/https or the user is fetching a non-http scheme.
- `macos9_fetcher_stubs_crlf.c` and `macos9_http_fetcher_crlf.c`, `_crlf` duplicates. Per the handoff these were removed from `MacSurf.mcp`; the files remain on disk but the project does not link them. If their MS_LOGs ever appear, MacSurf.mcp has a regression.

**fixes306 housekeeping:** the two `_crlf.c` files and the dead-register code in `macos9_ns_fetcher.c` are confusing on inspection. Worth a cleanup pass once page-load is unblocked.

## 4. Test cycle the user runs

Single, exact sequence:

1. **Empty Desktop of any prior `MacSurf Debug.log`.** We need to be sure the log we read is from this run.
2. Launch MacSurf.
3. Wait for the window to appear.
4. Click in the URL field and type `http://mac.mp.ls/simple.html`.
5. Press Return.
6. Wait 30 seconds (let any OT timeout fire, give scheduled callbacks time to drain).
7. Quit cleanly via File → Quit (or Cmd-Q if it works).
8. Open `MacSurf Debug.log` in SimpleText. Send the entire contents.

If quit is not clean, send the log anyway, `event loop exited` will be missing but everything before it is durable (per fixes149's flush-after-write design).

## 5. Decision tree, last log line → fixes306 scope

The diagnostic principle: **find the last MS_LOG line before the file ends.** That entry identifies the subsystem that broke. The case the log lands in locks the fixes306 scope.

### Case A, log ends before `initial window created`

Init regression. Not the page-load bug. Possible breakpoints:

- After `== MacSurf start ==`, before `InitOT OK` → OT init failure or crash. Suspect `kInitOTForApplicationMask` value.
- After `Appearance OK`, before `netsurf_register done` → `netsurf_register(&macos9_table)` crash. Suspect a NULL slot in the table (fixes287/288 fixed two; could be a third).
- After `netsurf_register done`, before `nsoption_init done` → `nsoption_init` crash. Default options init.
- After `nsoption_init done`, before `netsurf_init done` → `netsurf_init` crash. Fetcher factory or content factory init.
- After `netsurf_init done`, before `http_fetcher registered` → `macos9_http_fetcher_register` crash. lwc_intern or fetcher_add.
- After `http_fetcher registered`, before `initial window created` → `macos9_window_create` crash. TextEdit handle / control creation.

**fixes306 scope if Case A:** instrument the gap, then fix the broken init step. This is unrelated to fixes304, schedule_run never runs because we never reach the loop.

### Case B, `initial window created` is the last line, no `URL submit fired` after Return

Keyboard input doesn't reach the URL field. This is the **already-documented "URL field on initial window" bug** (CLAUDE.md "Current blockers"). 2026-04-18 survey hypothesis: content redraw during `browser_window_create` overdraws the URL rect while TE is functional internally.

**Workaround during the test cycle:** try `File → New Window`, click in that window's URL field, type, hit Return. If submit fires there but not on the initial window, this is the same bug, not a regression, Case B is confirmed and unrelated to fixes304.

**fixes306 scope if Case B:** dedicated probe round on the URL-rect overdraw. Add a one-shot probe in `plot_clip` / `plot_rectangle` logging coordinates that intersect `gw->url_rect` to confirm the hypothesis. Plotters.c already has the `plot_clip` MS_LOG infrastructure to extend.

### Case C, `URL submit fired`, then `submit: TEGetText null` or `submit: empty`

Address bar is reading nothing. TextEdit handle state is bad. Different bug from "fetcher inert."

**fixes306 scope if Case C:** TE handle lifecycle in `window.c` URL field setup. Check `g->url_te` is being initialized before the first submit.

### Case D, `nav: calling browser_window_navigate`, no `nav: done`

NetSurf core entered `browser_window_navigate` and didn't return. Synchronous crash or infinite loop inside core.

**fixes306 scope if Case D:** bisect inside `browser_window_navigate` with more MS_LOG. Hot suspects on this codebase: nsurl normalization, hlcache_handle_retrieve content-type lookup.

### Case E, `nav: done`, no `http_setup called`

This is the case fixes304 was supposed to fix. fetcher_poll wasn't running because `macos9_schedule_run()` wasn't being called from the event loop. fixes304 wires it at [main.c:185](../../browser/netsurf/frontends/macos9/main.c#L185).

**If Case E persists, fixes304 was wrong or insufficient.** Investigation queue for fixes306:

1. Confirm the binary actually has the fixes304 line. Grep the shipped main.c source on the Mac for `macos9_schedule_run`. If absent, the build didn't pick up the change.
2. Add MS_LOG inside [schedule.c:96 `macos9_schedule()`](../../browser/netsurf/frontends/macos9/schedule.c#L96) to confirm fetcher_poll is being inserted into the queue at all.
3. Add MS_LOG at the top of [schedule.c:141 `macos9_schedule_run()`](../../browser/netsurf/frontends/macos9/schedule.c#L141) to confirm the runner is firing every tick. Drop it before shipping, it'll be very noisy.
4. Verify [misc.c:62](../../browser/netsurf/frontends/macos9/misc.c#L62), `macos9_schedule` is in the right slot of `macos9_misc_table`. The handoff notes the cast on `macos9_gw_get_scroll` was masking a return-type mismatch (fixes296); same class of bug could apply here.
5. Check `macos9_quitting`, [schedule.c:151](../../browser/netsurf/frontends/macos9/schedule.c#L151) short-circuits the runner if it's set. If something sets it during init, the runner is permanently inert.

### Case F, `http_setup called` and `mfs_open called`, no visible page

Fetcher ran. OT request set up. Past the bug fixes304 addressed.

**fixes306 scope if Case F:** new instrumentation in [macos9_http_fetcher.c](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c) at the OT result-handling path, `OTSnd` / `OTRcv` returns and `fetch_send_callback(FETCH_HEADER | FETCH_DATA | FETCH_FINISHED)` calls. Goal: confirm bytes leave the box, response arrives, callback fires.

### Case G, pipeline completes, page renders but wrong

Out of scope for fixes305. Compare against `screenshots/v0.3-mactrove-fixes139.png`. Different bug class, layout / plotter regression rather than fetch inert.

## 6. Files most likely implicated (pre-test)

For fixes306 to target without re-discovery:

- [main.c:175-187](../../browser/netsurf/frontends/macos9/main.c#L175-L187), `macos9_poll` event-loop body. Schedule_run dispatch site.
- [schedule.c:96, 141, 151](../../browser/netsurf/frontends/macos9/schedule.c#L96), schedule insert, run, quitting-flag short-circuit.
- [misc.c:62](../../browser/netsurf/frontends/macos9/misc.c#L62), `macos9_misc_table.schedule` field assignment.
- [window.c:72-91](../../browser/netsurf/frontends/macos9/window.c#L72-L91), navigate + URL submit path.
- [macos9_http_fetcher.c:55, 141, 152, 158](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c), fetcher ops, register, `fetcher_add` for http/https.
- [plotters.c:200](../../browser/netsurf/frontends/macos9/plotters.c#L200), `plot_clip` instrumentation point for Case B URL-overdraw probe.

## 7. Hard rules respected this round

- **Markdown only.** No code shipped.
- **No staleness theories.** The user's build is what they say it is. Per CLAUDE.md Directive #1, every diagnosis path leads to a code-side root cause, not a transfer / cache narrative.
- **Init verified before instrumentation.** Per the Regression Audit Checklist, `grep -c macsurf_debug_log_init main.c` returns 1 at line 191. Channel is live, no fixes305a needed.
- **fixes304 is on probation, not assumed correct.** Cases E and F are written to confirm or refute it from the log evidence.
- **Documentation drift acknowledged but not patched mid-diagnosis.** CLAUDE.md edits are queued for fixes306+, not fixes305, so a regression discovered by the test cycle doesn't force a doc revert.
