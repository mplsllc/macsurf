# MacSurf `main.c` Audit — Carbon Startup Gaps

## Question
Read our `main.c` and list every initialization call we make before `netsurf_init` and before the window opens. Are we missing anything a proper Carbon app should call?

## Source: [browser/netsurf/frontends/macos9/main.c](../../browser/netsurf/frontends/macos9/main.c)

## Every init call MacSurf makes before the event loop

In order of execution inside `main()`:

1. `memset(&macos9_table, 0, sizeof(macos9_table));` — zero the netsurf dispatch table
2. Assign each sub-table into `macos9_table.*`
3. **Toolbox init block — entirely gated by `#ifndef TARGET_API_MAC_CARBON`:**
   ```c
   #ifndef TARGET_API_MAC_CARBON
       MaxApplZone();
       MoreMasters();
       InitGraf(&qd.thePort);
       InitFonts();
       InitWindows();
       InitMenus();
       TEInit();
       InitDialogs(NULL);
   #endif
   ```
   The prefix file `macsurf_prefix.h` defines `TARGET_API_MAC_CARBON 1`, so **every one of these calls is skipped** in the CW8 build.
4. `InitCursor();` — runs unconditionally
5. `FlushEvents(everyEvent, 0);` — runs unconditionally
6. `macos9_init_menus();` — builds the menu bar via `NewMenu` / `AppendMenu` / `InsertMenu` / `DrawMenuBar`
7. `netsurf_register(&macos9_table)`
8. `netsurf_init(NULL)`
9. `macos9_create_initial_window()` + first `macos9_fetch_url()`
10. Enter the `while (!macos9_done) { macos9_poll(); }` loop

## What MacSurf does **NOT** call that a proper Carbon app should

| Missing call | What Classilla does | Why it matters |
|---|---|---|
| **`'carb'` resource** | Has one (zero-length, ID 0) in several `.r` files in the tree | Without it, CFM does not recognize the binary as a Carbon app and CarbonLib does not fully engage. This is the most likely root cause of the `OTClientLib` crash. MacSurf's CW8 project has no `.r` file listed. |
| `RegisterAppearanceClient()` | Calls unconditionally (Gestalt-gated) in `CBrowserApp::CBrowserApp()` | Required for standard Carbon window controls to draw correctly. Not directly required by OT, but part of the standard Carbon app init contract. |
| `InstallCarbonEventHandlers()` | Calls under `#if TARGET_CARBON` in `CBrowserApp::CBrowserApp()` | Registers the Carbon Event Manager dispatch. MacSurf uses the classic `WaitNextEvent` loop, so strictly speaking this is optional — but it means MacSurf is a "Carbon app that opts out of Carbon events." |
| `OTOpenInternetServices(...)` before first `OTOpenEndpoint` | Does this in `_MD_FinishInitNetAccess()` before opening the first TCP endpoint | Classilla never opens a TCP endpoint without an Internet Services provider having been opened first. MacSurf jumps straight from `InitOpenTransportInContext` to `OTCreateConfiguration(kTCPName)`. Strong candidate for the fixed-address crash in `OTClientLib`. |
| `EnterMovies()` | Calls unconditionally if available | QuickTime init. Only needed if the browser decodes video. MacSurf does not — skipping is fine. |
| `AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, ...)` | Has dedicated AppleEvent handlers for quit/open/print | Without an AE quit handler, Carbon Finder quits will fall through MacSurf's `main.c` high-level-event branch which currently just logs and returns. Not a crash risk, but violates Carbon contract. |

### What MacSurf does that is **fine**

- `InitCursor()` — runs unconditionally, matches Classilla's `InitializeMacToolbox()` under classic (and is harmless under Carbon).
- `FlushEvents(everyEvent, 0)` — runs unconditionally. Classilla does **not** call this. It is not required on Carbon, but it is also not harmful. Keep or drop — no impact.
- Menu bar construction with `NewMenu`/`AppendMenu`/`DrawMenuBar` — standard; works under Carbon.
- The `WaitNextEvent` loop with a 1-tick sleep during fetches — correct cooperative-multitasking pattern.

### What MacSurf does that is **arguably wrong**

- **`#ifndef TARGET_API_MAC_CARBON` gating `InitGraf`/`InitFonts`/etc.** — matches Classilla (which also skips these under Carbon), so this is consistent with the reference. The comment in `main.c` says "Under Carbon these are not needed — Carbon initializes automatically" which is Apple's documented position for OS X, and Classilla's `"pinkerton - don't need to init toolbox under Carbon. They finally do that for us!"` supports it for OS 9 under CarbonLib.
- **But** this only works if the binary is actually launching as a Carbon app. Without a `'carb'` resource, the CFM loader runs the binary as classic, sees no `InitGraf` call, and leaves QuickDraw uninitialized — which is a disaster for everything that follows, including OT.

## Gap summary and rough priority

1. **No `'carb'` resource** — critical. Without it, CarbonLib is not fully engaged, and `*InContext` OT calls enter `OTClientLib` with uninitialized state. This alone is enough to explain a reproducible crash at a fixed address inside OTClientLib.
2. **No Internet Services provider opened before TCP endpoint** — high. Classilla enforces this explicitly.
3. **No `RegisterAppearanceClient()`** — medium. Not functionally required for OT per the source, but part of the Carbon app init contract Classilla follows.
4. **No `AEInstallEventHandler` for quit/open** — low. Affects Finder behavior only, not OT.
5. **No `InstallCarbonEventHandlers()`** — low. MacSurf opts out of Carbon events by using WaitNextEvent.
