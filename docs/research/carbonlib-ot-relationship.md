# CarbonLib / RegisterAppearanceClient / Open Transport Relationship

## Question
Does CarbonLib require `RegisterAppearanceClient()` to be called before OT will work correctly? Is there a known issue where CarbonLib on OS 9 wasn't fully initialized for apps that didn't register with the Appearance Manager?

## Short answer

There is **no direct evidence in the Classilla source tree** that `RegisterAppearanceClient()` is a prerequisite for Open Transport. Classilla calls them in a fixed order (Appearance first, then networking later via the event loop), but the two subsystems are architecturally independent.

The real gating relationship, based on what is in the Classilla source tree, is different and more specific:

**OT needs a `'carb'` (or `'plst'`) resource for CarbonLib to fully engage on OS 9, and it needs an Internet Services provider to be opened before any TCP endpoint.**

## What the Classilla source shows

### 1. Appearance and OT live in different layers
- `RegisterAppearanceClient()` is called in `CBrowserApp::CBrowserApp()` (the application object's constructor), under a `Gestalt(env_HasAppearance)` check.
- `InitOpenTransportInContext()` is called in `macsockotpt.c`'s `_MD_InitNetAccess()`, which runs lazily when NSPR first creates a socket.
- These are entirely separate call stacks. There is no direct dependency from the OT code on anything the Appearance Manager does.

### 2. The only shared dependency is CarbonLib itself
Both subsystems live inside CarbonLib on OS 9:
- `RegisterAppearanceClient` is in the Appearance Manager which CarbonLib wraps.
- `InitOpenTransportInContext`, `OTCreateConfiguration`, `OTOpenEndpointInContext` are in `OTClientLib`, which CarbonLib loads.

If CarbonLib is not fully engaged (because the app is not recognized as a Carbon app), **neither** will work correctly. The symptom in either case is a crash at a fixed address inside the relevant CarbonLib sub-library.

### 3. Classilla's ordering is "safety in ordering" not "functional dependency"

Looking at the actual source, the order is:

1. `InitializeMacToolbox()` — skipped under Carbon, placeholder under Carbon
2. `CBrowserApp::CBrowserApp()` constructor:
   - `InstallCarbonEventHandlers()` (Carbon only)
   - `RegisterAppearanceClient()` (Gestalt-gated, both Carbon and classic)
   - `RegisterClass_(...)` / PowerPlant class registration
3. `theApp.Run()` → event loop starts
4. XPCOM initialization on first request → `_MD_InitNetAccess()` → `InitOpenTransportInContext()`
5. First TCP socket → `_MD_FinishInitNetAccess()` → `OTOpenInternetServices(...)` then `OTOpenEndpointInContext(...)`

OT initialization is very late in this chain — triggered by the first network request, long after Appearance and Carbon Event Manager are set up. But the cause isn't "Appearance must come first"; it's "by the time OT is used, the app has been fully running as a Carbon app for a while, so CarbonLib is definitely engaged."

## Is there a documented Apple bug linking the two?

Not in the Classilla source tree. No `grep` for "appearance" near "OT" / "opentransport" turned up anything, and no file named or commented to suggest this dependency.

The closest thing in public record (and outside the Classilla source tree, so not verified in this session) is Apple's historical guidance that Carbon apps on OS 9 need to:

1. Have a `'carb'` or `'plst'` resource for CarbonLib to load.
2. Register with the Appearance Manager before creating windows that use Appearance features (standard Carbon window types).

Neither of these is an OT-specific rule — they are just Carbon app rules. But violating either one on OS 9 produces "CarbonLib isn't fully set up" symptoms, which affect **every** CarbonLib subsystem including `OTClientLib`.

## What is a real prerequisite for OT (from Classilla's `macsockotpt.c`)

Separate from the Carbon/Appearance question, Classilla's `_MD_socket()` enforces that **Internet Services must be opened before a TCP endpoint**:

```c
PRInt32 _MD_socket(int domain, int type, int protocol)
{
    ...
    _MD_FinishInitNetAccess();   /* opens OTOpenInternetServices first */
    ...
    /* only then does CreateSocket() call OTCreateConfiguration(kTCPName) + OTOpenEndpointInContext */
}
```

`_MD_FinishInitNetAccess()` opens an Internet Services provider (for DNS), installs a notifier on it, switches it to async, and **keeps it open for the process lifetime**. Then and only then does Classilla open TCP endpoints.

This is the most concrete "prerequisite call before opening an endpoint" pattern in the Classilla source. If there is a functional reason MacSurf's current code crashes inside `OTClientLib`, this is more likely to be it than anything Appearance-related.

## Conclusion

1. `RegisterAppearanceClient()` is **not** a documented or source-visible prerequisite for OT in Classilla.
2. What Classilla *does* guarantee before OT runs is: a `'carb'`/`'plst'` resource in the binary, a Carbon-aware `main()`, and an Internet Services provider opened before any TCP endpoint.
3. For MacSurf, the highest-confidence fixes are (in order): add a `'carb'` resource; open an Internet Services provider once before opening the TCP endpoint.
4. Adding `RegisterAppearanceClient()` is cheap, defensive, and matches what Classilla does — but should be framed as "match the reference app's init sequence" rather than "required for OT."
