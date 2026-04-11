# Classilla: Startup Sequence Before Networking

## Questions
- What does Classilla's startup look like before any networking happens? What Carbon/Toolbox init calls run in `main()`?
- Does it call `RegisterAppearanceClient` or other Carbon registration calls?
- Does it call `FlushEvents` at startup?

## Sources read
- `/tmp/classilla/mozsrc/mozilla/embedding/browser/powerplant/source/CBrowserApp.cp` — PPEmbed sample Mozilla Carbon browser
- `/tmp/classilla/mozsrc/mozilla/lib/mac/NSStdLib/src/macstdlibextras.c` — the shared `InitializeMacToolbox()` used by all Classilla/Mozilla Mac builds
- `/tmp/classilla/mozsrc/mozilla/widget/src/mac/nsAppShell.cpp` — the widget-layer app shell, Carbon-aware

## The full sequence

### 1. Process entry (`main()` in CBrowserApp.cp)

```cpp
int main()
{
    SetDebugThrow_(PP_PowerPlant::debugAction_Alert);
    SetDebugSignal_(PP_PowerPlant::debugAction_Alert);

    PP_PowerPlant::InitializeHeap(3);          // Memory Manager
    PP_PowerPlant::UQDGlobals::InitializeToolbox();   // calls InitializeMacToolbox() below

#if !TARGET_CARBON
    new PP_PowerPlant::LGrowZone(20000);
    ::InitTSMAwareApplication();
#endif

    {
        CBrowserApp theApp;                    // constructor runs the Carbon registration
        theApp.Run();                          // enters the event loop — XPCOM and networking start here
    }
    return 0;
}
```

Networking does **not** run until `theApp.Run()` enters the event loop. Everything before that is Toolbox + Carbon setup.

### 2. `InitializeMacToolbox()` (in `macstdlibextras.c`)

This is the shared Toolbox-init routine that `UQDGlobals::InitializeToolbox()` ultimately calls:

```c
void InitializeMacToolbox(void)
{
    static Boolean alreadyInitialized = false;
    if (!alreadyInitialized) {
        long CMMavail = 0;
        alreadyInitialized = true;

#if !TARGET_CARBON
        /* "pinkerton - don't need to init toolbox under Carbon. They finally do that for us!" */
        InitGraf(&qd.thePort);
        InitFonts();
        InitWindows();
        InitMenus();
        TEInit();
        InitDialogs(0);
        InitCursor();

        Gestalt(gestaltContextualMenuAttr, &CMMavail);
        if ((CMMavail == gestaltContextualMenuTrapAvailable) &&
            ((long)InitContextualMenus != kUnresolvedCFragSymbolAddress))
            InitContextualMenus();

        InitTSMAwareApplication();
#endif

        if ((long)EnterMovies != kUnresolvedCFragSymbolAddress)
            EnterMovies();                     // QuickTime — runs under BOTH classic and Carbon

#if DEBUG
        InitializeSIOUX(false);
#endif
    }
}
```

**Important:** every one of `InitGraf` / `InitFonts` / `InitWindows` / `InitMenus` / `TEInit` / `InitDialogs` / `InitCursor` / `InitContextualMenus` / `InitTSMAwareApplication` is gated by `#if !TARGET_CARBON`. Classilla skips them under Carbon, relying entirely on CarbonLib's automatic Toolbox initialization. The comment is explicit: `"pinkerton - don't need to init toolbox under Carbon. They finally do that for us!"`

**`EnterMovies()` is the one call that runs under both.** It is called unconditionally (guarded only by CFM symbol availability). QuickTime needs explicit initialization even under Carbon.

### 3. `CBrowserApp::CBrowserApp()` constructor — Carbon registration lives here

```cpp
CBrowserApp::CBrowserApp()
{
#if TARGET_CARBON
    InstallCarbonEventHandlers();              // Carbon Event Manager dispatch table
#endif

    if (PP_PowerPlant::UEnvironment::HasFeature(PP_PowerPlant::env_HasAppearance)) {
        ::RegisterAppearanceClient();          // <-- YES, Classilla calls it
    }

    RegisterClass_(...);                       // PowerPlant class registration
    PP_PowerPlant::UControlRegistry::RegisterClasses();
    UQuickTime::Initialize();
    RegisterClass_(CBrowserShell);
    ...
}
```

### 4. `nsAppShell::nsAppShell()` constructor (widget layer)

```cpp
nsAppShell::nsAppShell()
{
#if TARGET_CARBON
    mInitializedToolbox = PR_TRUE;             // just set the flag, Carbon does it for us
#else
    if (!mInitializedToolbox) {
        InitializeMacToolbox();                // fall through to the classic init
        mInitializedToolbox = PR_TRUE;
    }
#endif
    mRefCnt = 0;
    mExitCalled = PR_FALSE;
}
```

Same pattern: skip everything under Carbon, call `InitializeMacToolbox()` otherwise.

## Answers to each sub-question

### "Does it call `RegisterAppearanceClient`?"

**Yes, explicitly, in `CBrowserApp`'s constructor, guarded by a Gestalt check for `env_HasAppearance`.** The call is unconditional under Carbon (no `#if TARGET_CARBON` wrapper) — Classilla calls it on OS 9 and OS X alike.

`grep -rln RegisterAppearanceClient /tmp/classilla/mozsrc/mozilla` found only `embedding/browser/powerplant/source/CBrowserApp.cp`. It is the single call site, but it is the main entry point for the browser application.

Placement: **after** Toolbox init, **before** the event loop / networking. Every Classilla session runs `RegisterAppearanceClient` before any fetch runs.

### "Does it call `FlushEvents`?"

**Not in any file under `mozilla/lib/mac`, `mozilla/xpfe/bootstrap`, or `mozilla/embedding`.** `grep -rn FlushEvents` across those directories returned zero matches. Classilla does not drain the event queue with `FlushEvents(everyEvent, 0)` at startup.

(This contradicts the conventional Toolbox cookbook advice but is consistent with modern Carbon practice: the Carbon Event Manager dispatcher handles stale events naturally.)

### "What Carbon/Toolbox init calls happen before networking?"

Summary table:

| Call | Under Carbon | Under Classic | Notes |
|---|---|---|---|
| `InitializeHeap(3)` | ✓ | ✓ | PowerPlant memory manager — called unconditionally |
| `InitGraf` / `InitFonts` / `InitWindows` / `InitMenus` / `TEInit` / `InitDialogs` / `InitCursor` | ✗ | ✓ | Skipped under Carbon |
| `InitContextualMenus` | ✗ | ✓ (if available) | Skipped under Carbon |
| `InitTSMAwareApplication` | ✗ | ✓ | Skipped under Carbon |
| `LGrowZone(20000)` | ✗ | ✓ | PowerPlant low-memory handler — skipped under Carbon |
| `EnterMovies()` | ✓ | ✓ | QuickTime — always called |
| `InstallCarbonEventHandlers()` | ✓ | ✗ | Carbon Event Manager — only under Carbon |
| `RegisterAppearanceClient()` | ✓ | ✓ (if available) | Called unconditionally, gated only by Gestalt check |
| `InitializeSIOUX(false)` | ✓ (DEBUG only) | ✓ (DEBUG only) | Stdio console under debug builds |
| `FlushEvents(everyEvent, 0)` | — | — | **Not called at all** |

## Takeaway

Classilla's startup under Carbon on OS 9 is actually quite thin:

1. `InitializeHeap` (PowerPlant)
2. `EnterMovies` (QuickTime, if available)
3. `InstallCarbonEventHandlers` (Carbon Event Manager)
4. `RegisterAppearanceClient` (Appearance Manager, gated on Gestalt)
5. Class registration + QuickTime registration
6. Enter the event loop

Everything else (InitGraf, FlushEvents, LGrowZone) is deliberately skipped under Carbon.

The steps MacSurf may be missing relative to Classilla are: `EnterMovies` (probably not needed for a browser without video), `InstallCarbonEventHandlers` (we use WaitNextEvent directly, so not applicable), and **`RegisterAppearanceClient()`** (Appearance Manager registration — not done in our `main.c` at all).
