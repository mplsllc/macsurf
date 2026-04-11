# Classilla: `'carb'` Resource

## Question
Does Classilla have a `'carb'` resource in its binary? How is it set up?

## Answer
**Yes.** Multiple `.r` files in the Classilla source tree define a zero-length `'carb'` resource. It is the Classic Mac OS marker that tells the Code Fragment Manager / CarbonLib: "this CFM fragment is a Carbon application — load CarbonLib and route Toolbox calls through it."

## Where it appears

Found in the Classilla source at `/tmp/classilla/mozsrc/`:

1. **`mozilla/js/src/macbuild/carbon.r`** (one line, the canonical form):
   ```rez
   type 'carb' {}; resource 'carb'(0) {};
   ```

2. **`mozilla/xpinstall/cleanup/macbuild/CarbResource.r`** (same with comments + name):
   ```rez
   type 'carb' {};
   resource 'carb' (0, "") {};
   ```

3. **`mozilla/xpfe/bootstrap/carbon.r`** (the main browser's variant — uses OS X–style `'plst'` instead):
   ```rez
   read 'plst' (0) "mozilla.plst";
   ```
   The browser bootstrap reads an external `mozilla.plst` (Info.plist) file as a `'plst'` resource. On OS X, `'plst'` is the equivalent of `'carb'`. On pure OS 9 Classilla builds, the `'carb'` variant is used instead.

## What the `'carb'` resource *does*

- It is a zero-length resource. Its contents don't matter — **only its presence matters**.
- CFM inspects it at application launch. If present, CFM loads `CarbonLib` (the Carbon shared library) and the application runs in Carbon mode. Without it, the fragment is treated as a classic PEF application.
- Under Carbon mode, calls like `InitOpenTransportInContext`, `OTOpenEndpointInContext`, and every `*InContext` variant are resolved through CarbonLib's `OTClientLib` wrappers rather than the classic OT shared library.
- Without a `'carb'` resource, calling `InitOpenTransportInContext` from a classic-mode app still resolves (because the symbol is in CarbonLib), **but CarbonLib's internal state may not be properly initialized**, and subsequent `OTClientLib` calls can crash at fixed addresses inside the library.

## How it is compiled in

The `.r` files are fed to Rez (MPW) or CodeWarrior's Rez compiler and linked into the application's resource fork alongside `MENU`, `DLOG`, `WIND`, and other resources. In a CodeWarrior project, the `.r` file is added to the project alongside `.c` files; CW builds the resource fork automatically.

## Implications for MacSurf

The MacSurf project as of 2026-04-11 does **not** contain a `.r` file with a `'carb'` resource. The CW8 project (`MacSurf.mcp`) has no `.r` file listed in its project contents. If MacSurf's binary has no `'carb'` or `'plst'` resource, then **the application is not being launched as a Carbon app** — CFM treats it as classic, CarbonLib does not fully engage, and any `*InContext` OT call runs on an uninitialized OT client context.

This is the single most likely cause of the OTClientLib crash observed at the fixed address.
