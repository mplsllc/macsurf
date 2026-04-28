# fixes305 — page-load diagnosis (no code changes)

Round purpose: diagnostic only. Reconcile documentation gap, verify the
file-backed log channel is wired, then have the user run a test cycle
on hardware and read the log. Code changes are deferred to fixes306.

Last shipped (per handoff): **fixes304** — wired `macos9_schedule_run()`
into `macos9_poll`. Untested by user.

---

## 1. CLAUDE.md vs. fixes260–304 handoff — reconciliation

CLAUDE.md "Last shipped fix" line still says **fixes266**.
Actual last shipped fix is **fixes304** (45-fix gap: fixes267–304 are
not represented in the project doc).

The CLAUDE.md "Build State" section still describes the v0.3 native CSS
custom-property milestone with MacTrove rendering — that history is
correct as a v0.3 description, but does not capture that the project
has since gone through:

- A massive Carbon.h `#include` chain pruning campaign (fixes260–269).
- The libcss `_ALIGNED` keyword cascade and 20+ headers gaining
  defensive `#define _ALIGNED` (fixes271–277).
- Wholesale replacement of `*_stub.c` files with real ports
  (fixes278–286). MacSurf.mcp's project file list changed materially
  in this phase — see handoff §"Project file list changes during the
  sprint."
- Discovery and fix of the **MSL Console shadow bug** (fixes298) which
  was the actual "main never enters" root cause across the run-to-Play
  sessions in fixes294–296.
- `'carb'` resource regression: `MacSurf.rsrc` (binary) silently fails
  through CW8's File Mappings (which routes `.rsrc` extension to the
  Rez compiler). Real fix is `MacSurf.r` + `Types.r` Rez sources
  (fixes297). CLAUDE.md's "Carbon App Requirements" still describes
  the binary-`.rsrc` path as canonical.
- Discovery that `InitOpenTransport()` is **not exported** by this
  CarbonLib build, contradicting CLAUDE.md "Open Transport Rules" which
  recommend the plain (non-`InContext`) variants. Current code uses
  `InitOpenTransportInContext` with manual `#define
  kInitOTForApplicationMask 0x00000002` (fixes289–291).
- `macos9_schedule_run()` wiring into the event-loop poll (fixes304),
  which CLAUDE.md does not mention as a hard requirement for fetcher
  callbacks to drain.

Documentation update is queued for fixes306+ once page-load behavior
is confirmed. **Do not update CLAUDE.md inside fixes305**; the round is
diagnostic and shouldn't ship a new doc state we then have to revert
if the load failure exposes a regression.

Concrete CLAUDE.md edits queued for after fixes306 closes:

1. "Last shipped fix" → fixes306 (or whatever closes the loop).
2. "Carbon App Requirements" — replace "compile binary `.rsrc`" with
   "ship `MacSurf.r` + `Types.r` Rez sources, do not add binary
   `.rsrc` files because CW8 File Mappings routes `.rsrc` to Rez."
3. "Open Transport Rules" — annotate "plain OT calls" as the
   architectural intent, but document that this CarbonLib only exports
   `*InContext` variants, so current code uses InContext + the
   `kInitOTForApplicationMask 0x00000002` manual define.
4. New "Don't shadow MSL" gotcha — `InstallConsole`, `RemoveConsole`,
   `Read/WriteCharsToConsole`, `strdup`, `strcasecmp`, `strncasecmp`,
   `mkdir`, `stat` are provided by `MSL_All_Carbon.Lib`. Stubbing them
   silently corrupts `FILE*` table init and crashes before `main()`.
5. New "Schedule runner is mandatory" gotcha — `macos9_schedule_run()`
   must be called every event-loop tick. Without it, fetcher_poll
   self-schedules but never drains.
6. Build state section — bump v0.3 description to note the post-sprint
   state (libcss/libdom/libhubbub/libparserutils real ports in
   MacSurf.mcp; many `*_stub.c` removed; MSL_All_Carbon shadowing rule
   in effect).

## 2. File-backed log channel — verification

Per the Regression Audit Checklist in CLAUDE.md, every shipped
subsystem must satisfy three things: init wired, smoke confirmed,
documented dependency. Verifying the log channel against this rule:

| Requirement | Status |
|---|---|
| `macsurf_debug_log_init()` symbol exists | ✓ [macsurf_debug_log.c:206](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L206) |
| Init call wired into `main()` | ✓ [main.c:191](../../browser/netsurf/frontends/macos9/main.c#L191) — first executable line in `main()`, before any other init |
| `MS_LOG` macro wired through to log file | ✓ macsurf_debug.h forwards to `macsurf_debug_log_write*` |
| Init dependencies documented in CLAUDE.md | ✓ "Regression Audit Checklist" table includes the entry for "File-backed diagnostic log" |

Conclusion: the channel **is** wired and should populate
`MacSurf Debug.log` on the Desktop on every launch. No fixes305a hotfix
needed.

If the user runs a launch and the log file does **not** appear on the
Desktop, the failure mode is one of:

- `FindFolder(kOnSystemDisk, kDesktopFolderType, ...)` returning an
  error — log init silently inert (per CLAUDE.md's "File-Backed
  Diagnostic Channel" section).
- `FSpCreate` failing because the volume is full / locked / read-only.
- Crash before `main.c:191` runs at all — i.e. CFM startup or MSL init
  blowing up. This was the failure shape in fixes294–298 (the
  MSL-Console shadow bug). If it returns, suspect newly added
  stubs or new Console-touching code.

## 3. Pipeline trace — what each MS_LOG entry means

Current MS_LOG checkpoints in code (from `git grep MS_LOG`):

```
main.c:192   "== MacSurf start =="            entry into main
main.c:199   "InitOT FAIL"                    Open Transport init failure
main.c:202   "InitOT OK"                      Open Transport ready
main.c:205   "Appearance OK"                  RegisterAppearanceClient returned
main.c:222   "netsurf_register done"          gui table registered
main.c:224   "nsoption_init done"             options system live
main.c:226   "netsurf_init done"              core init complete
main.c:230   "http_fetcher registered"        http: scheme handler installed
main.c:233   "initial window created"         first window up
main.c:235   "event loop exited"              main loop returned

window.c:72  "navigate:"                      browser_window_navigate path entered
window.c:73  <url string>                     URL being navigated to
window.c:74  "nav: no g or empty u"           bailout: missing window or URL
window.c:75  "nav: no bw"                     bailout: window has no browser_window yet
window.c:79  "nav: nsurl_create FAIL"         URL parsing failed
window.c:80  "nav: calling browser_window_navigate"  about to enter NetSurf
window.c:83  "nav: done"                      browser_window_navigate returned

window.c:88  "URL submit fired"               Return key in URL field captured
window.c:89  "submit: no g or url_te"         bailout: missing TextEdit handle
window.c:90  "submit: TEGetText null"         TextEdit returned no buffer
window.c:91  "submit: empty"                  TextEdit buffer was zero-length

macos9_http_fetcher.c:55  "mfs_open called"   http_setup → mfs_open transition
macos9_http_fetcher.c:141 "http_setup called" fetcher_operation_table::setup invoked
```

## 4. What the user should do

Single test cycle on hardware, exactly:

1. Empty Desktop of any prior `MacSurf Debug.log` (so we can be sure the
   log we read is from this run).
2. Launch MacSurf.
3. Wait for the window to appear.
4. Click in the URL field, type `http://mac.mp.ls/simple.html`.
5. Press Return.
6. Wait 30 seconds (let any OT timeout fire).
7. Quit cleanly via File → Quit (or Cmd-Q if it works).
8. Open `MacSurf Debug.log` in SimpleText. Send the entire contents back.

## 5. How to read the log (decision tree)

Find the **last MS_LOG line before the file ends**. That entry
identifies the subsystem that owns the page-load break.

### Case A — log ends at startup (no `initial window created`)

Init failure, not a page-load failure. Not the bug we're chasing —
report and stop. Likely culprits if this happens:

- "InitOT FAIL" — `kInitOTForApplicationMask` mismatch or OT
  not loadable at all under this CarbonLib version.
- Missing entry between `netsurf_register done` and `nsoption_init done`
  → nsoption init crashes, possibly because options.h vars aren't
  initialized.
- Missing entry between `netsurf_init done` and `http_fetcher
  registered` → fetcher register crash.

### Case B — log ends at `initial window created` and never gains anything when user types

The keyboard input path doesn't reach the URL field. The user is typing
but `URL submit fired` is never logged on Return.

Suspects (in order of likelihood):
- TextEdit field never activated for the initial window. CLAUDE.md
  already documents this exact bug in "Current blockers — feature
  gaps" as "URL field input fails on the initial window, works on
  File → New Window." Hypothesis from 2026-04-18 survey: content
  redraw during `browser_window_create` overdraws the URL rect while
  TE is functional internally.
- Workaround for the user's test cycle: try `File → New Window`, then
  type URL there. If submit fires from the new window but not the
  initial one, this is the same bug, not a regression.

If the user's test bypasses this by using `File → New Window` and
**still** sees no progress past `URL submit fired`, see Case C.

### Case C — `URL submit fired` logged, no `navigate:` follows

The submit handler bails before calling `macos9_window_navigate`.
Probable cause: `submit: TEGetText null` or `submit: empty`. Both will
appear in the log. The address bar is reading nothing — TextEdit handle
state is bad. Different bug from "fetcher inert."

### Case D — `nav: calling browser_window_navigate` logged, no `nav: done`

NetSurf entered `browser_window_navigate` and did not return. This
means a synchronous crash or an infinite loop inside core. Compare
against pre-sprint MacTrove behaviour: a fresh-window navigate should
return promptly (the actual fetch is async via the scheduler).

### Case E — `nav: done` logged, but **no** `http_setup called` ever appears

This is the fetcher-inert case fixes304 was supposed to fix.

If fixes304 is in the build (line 185 of `main.c` reads
`{ extern bool macos9_schedule_run(void); macos9_schedule_run(); }`)
and the log still shows `nav: done` with no subsequent `http_setup
called`, fixes304 did **not** fix the issue. Next steps in fixes306:

- Verify `macos9_misc_table.schedule == macos9_schedule` is taking
  effect — log inside `macos9_schedule()` to confirm fetcher_poll is
  being inserted. (Currently no instrumentation there.)
- Verify the queue head's `time` isn't in the past relative to
  `TickCount()` due to a sign / wraparound bug.
- Verify `macos9_quitting` isn't getting set early — line 151 of
  `schedule.c` short-circuits the runner if so.

### Case F — `http_setup called` and `mfs_open called` appear, but page never visibly renders

The fetcher ran. The OT request was set up. We're past the bug fixes304
addressed. Next instrumentation belongs inside `macos9_http_fetcher.c`
at the OT result-handling path (`OTSnd` / `OTRcv` returns,
`fetch_send_callback(FETCH_HEADER|FETCH_DATA|FETCH_FINISHED)` calls).

### Case G — fetcher pipeline completes, but render is wrong

Out of scope for fixes305. Compare against
`screenshots/v0.3-mactrove-fixes139.png` to confirm whether layout has
regressed or whether something specific to `simple.html` is wrong.

## 6. Files implicated by current evidence (pre-test)

These are the files that own the most likely failure points. Listed so
fixes306 can target them directly without re-discovering during the
next round:

- [main.c:175-188](../../browser/netsurf/frontends/macos9/main.c#L175-L188) — `macos9_poll`,
  the schedule_run dispatch site.
- [schedule.c:140-173](../../browser/netsurf/frontends/macos9/schedule.c#L140-L173) — `macos9_schedule_run` itself.
- [misc.c:61-68](../../browser/netsurf/frontends/macos9/misc.c#L61-L68) — `macos9_misc_table` field order; if `schedule` is at the wrong slot, NetSurf calls the wrong function pointer.
- [window.c:72-83](../../browser/netsurf/frontends/macos9/window.c#L72-L83) — navigate path.
- [macos9_http_fetcher.c:55,141](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c) — fetcher entry points.
- [macsurf_debug_log.c:206-275](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L206) — log init (verified wired).

## 7. Recommended fixes306 scope (contingent on log)

Cannot be locked in until we read the log. The candidates by case:

| Log stops at | fixes306 owner |
|---|---|
| Pre-`initial window created` | Init regression — fix where the chain breaks. |
| `initial window created`, no submit fires | URL-field-on-initial-window bug (already documented). Probe `plot_clip`/`plot_rectangle` for URL-rect overdraws. |
| `URL submit fired`, no `navigate:` | TextEdit handle / submit guard in `window.c`. |
| `nav: calling browser_window_navigate`, no `nav: done` | NetSurf core hang or crash on nsurl. Bisect via more MS_LOG inside `browser_window_navigate`. |
| `nav: done`, no `http_setup called` | fixes304 didn't fix; instrument `macos9_schedule` insertion path and verify queue draining. |
| `http_setup called`, no `mfs_open called` | OT setup error — check `macos9_http_setup` return path. |
| `mfs_open called`, blank page | OT recv path or fetch_send_callback wiring; new instrumentation in fetcher result path. |

## 8. Hard rules respected this round

- No code shipped (markdown only).
- No staleness theories. The user's build is what they say it is.
- Init wiring verified before recommending instrumentation. Per the
  Regression Audit Checklist, the file-backed log was confirmed wired
  by `grep -c macsurf_debug_log_init main.c` returning ≥1 (it returns
  1, at line 191).
- The diagnosis explicitly does not assume fixes304 was correct.
  Cases E and F above are written to confirm or refute it.
