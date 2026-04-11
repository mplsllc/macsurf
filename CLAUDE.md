# MacSurf

A lightweight web browser for Mac OS 9 PowerPC, built on the NetSurf engine, paired with a simple TLS proxy.

## Project Structure

```
macsurf/
├── browser/          # NetSurf fork with macos9 frontend
├── proxy/            # MacSurf Proxy (Go)
└── docs/             # Build and deployment docs
```

## Two Components

### 1. MacSurf Browser
A port of NetSurf to Classic Mac OS 9 using the Carbon API and CodeWarrior 8. Cross-compiled from Linux targeting PowerPC. Tabs disabled by default. Proxy config built into preferences.

### 2. MacSurf Proxy
A single Go binary that strips TLS — receives plain HTTP from the Mac, fetches via HTTPS, returns plain HTTP. No config files, no dependencies. Deployable on a VPS or local machine.

## Key Technical Constraints

- Target: Power Mac G3/G4, Mac OS 9.1-9.2.2, minimum 64MB RAM
- Compiler: CodeWarrior 8 (on-machine) or cross-compile GCC PPC from Linux
- No threading — OS 9 is cooperative multitasking, use WaitNextEvent loop
- No HTTPS in browser — all TLS handled by proxy
- No JavaScript — by design, keeps memory footprint low
- Carbon API for UI — works on OS 9 and early OS X

## Coding Conventions

- C for browser frontend (matches NetSurf codebase)
- Go for proxy
- Keep Mac Toolbox calls isolated in their own files (window.c, bitmap.c, font.c etc.)
- No external dependencies in proxy — stdlib only

## Carbon App Requirements

MacSurf is a Carbon CFM app running under CarbonLib on OS 9. For CarbonLib to fully engage, the binary MUST be identifiable as a Carbon fragment — otherwise `*InContext` calls crash at fixed addresses inside OTClientLib.

- **`'carb'` resource is mandatory.** Without it, CFM treats the binary as classic PEF, CarbonLib does not load as a dependency, and any `*InContext` OT call enters an uninitialized CarbonLib client context and crashes. This is the single most important requirement for a Carbon app on OS 9.
- **`MacSurf.r`** contains the `'carb'` resource (zero-length, ID 0). The `.r` file must be listed in the CW8 project alongside the `.c` files so CW's Rez step builds it into the resource fork.
- **`RegisterAppearanceClient()`** must be called at startup after `InitCursor()`, gated by a Gestalt check for Appearance Manager presence. Matches Classilla's `CBrowserApp` constructor pattern.
- **Skip** `InitGraf`/`InitFonts`/`InitWindows`/`InitMenus`/`TEInit`/`InitDialogs` under Carbon — Classilla explicitly skips them and so should MacSurf. Keep `InitCursor()` and `FlushEvents(everyEvent, 0)`.
- **No preemptive threads.** OS 9 is cooperative. Use `WaitNextEvent` for the UI event loop. OT yields happen through the notifier callback (see below).

## Open Transport Rules

MacSurf uses **plain (non-`InContext`) Open Transport calls**. This is verified against the Retro68 OT TCP demo and SSHeven, both of which run on real OS 9.2 hardware.

- Use `InitOpenTransport()`, `OTOpenEndpoint()`, `CloseOpenTransport()` — **not** the `*InContext` variants. The InContext variants route through CarbonLib's OTClientLib, which has been the source of every crash we've seen.
- Use `OTUseSyncIdleEvents(ep, true)` plus a notifier that calls `YieldToAnyThread()` on `kOTSyncIdleEvent`. This is the cooperative-multitasking answer for synchronous OT calls — OT fires `kOTSyncIdleEvent` periodically while blocked, the notifier yields to the Thread Manager, and the app stays responsive without touching `WaitNextEvent` from inside the fetch.
- Use `OTInitDNSAddress(&dnsAddr, "host:port")` for address setup — one string, OT resolves hostname and port. Simpler than `OTInetStringToHost` + `OTInitInetAddress`.
- `OTBind(ep, NULL, NULL)` is legal and correct. No TBind ret buffer needed for outbound-only TCP clients.
- Include `<Threads.h>` — the classic Thread Manager is required for `YieldToAnyThread`.
- Reference implementations: [cy384/ssheven](https://github.com/cy384/ssheven) (production SSH client) and [cy384/miscellany retro68-demos/ot-tcp-demo.c](https://github.com/cy384/miscellany) (Apple `OTSimpleDownloadHTTP.c` adapted for Retro68). Both verified on OS 9.2.

## Prior Art

- **MacSurf appears to be the first serious NetSurf port to Classic Mac OS.** The netsurf-dev list has a single 2017 "Port to OS9?" thread with no follow-through. There is no prior NetSurf OS 9 port to reference.
- **Best networking references:**
  - [Classilla](https://sourceforge.net/projects/classilla/) — `macsockotpt.c` (NSPR's OT sockets layer) and `directory/c-sdk/ldap/libraries/macintosh/tcp-univhdrs/tcp.c` (standalone TCP over OT). Full Mozilla-era Carbon browser running on OS 9.
  - [cy384/ssheven](https://github.com/cy384/ssheven) — modern production SSH client, cooperative thread + OT.
  - [cy384/miscellany `retro68-demos/ot-tcp-demo.c`](https://github.com/cy384/miscellany) — shortest known-good OT HTTP client, ~220 lines.
- **Not references:** iCab (closed source), WaMCom (Classilla predecessor, same codebase), MoonlightOS (does not exist as far as we can find).

## Reference Frontends

NetSurf's RISC OS and AmigaOS frontends are the primary references for frontend architecture — both solved cooperative multitasking on non-POSIX systems. Study these before writing any frontend code.

- `frontends/riscos/` — closest analog to Mac OS 9
- `frontends/amiga/` — also cooperative multitasking

## Proxy Protocol

Standard HTTP proxy protocol — no custom protocol. The Mac sends a normal HTTP proxy request, the proxy fetches via HTTPS and returns plain HTTP. Compatible with any browser that supports HTTP proxies (Classilla works today as validation).

## Do Not

- Do not add JavaScript support
- Do not enable tabs by default
- Do not use preemptive threads anywhere in the browser
- Do not add external dependencies to the proxy
- Do not target OS X only — Carbon must run on OS 9

## Build Environment

### Compiler
- CodeWarrior 8 Pro (with 8.3 update) running on Mac OS 9
- CW8 compiles in C89 mode — no C99, no C++ features
- CW8 defines `__MWERKS__` — use this to detect the compiler
- The project defines `__MACOS9__ 1` via the prefix file `macsurf_prefix.h`
- CW8 does NOT support: `inline`, `//` comments, variadic macros, forward enum declarations, C99 designated initializers, `for (int i...)`

### Prefix File
`browser/netsurf/frontends/macos9/macsurf_prefix.h` is injected before every compilation unit. It currently defines:
- `__MACOS9__ 1`
- `WITHOUT_DUKTAPE 1`
- `NO_IPV6 1`
- `TARGET_API_MAC_CARBON 1`
- `#include <MacTypes.h>` (first line — must stay first to prevent bool/true/false conflict)

### Shims Layer
POSIX functionality is provided by stubs in `browser/netsurf/frontends/macos9/shims/`. These must be C89 compatible. Mac Toolbox headers must always be included before any bool/true/false definitions.

### Stub Headers
External dependencies not available on OS 9 are stubbed in `browser/netsurf/frontends/macos9/`:
- `libwapcaplet/libwapcaplet.h`
- `dom/dom.h`
- `libcss/libcss.h`
- `nsutils/endian.h`, `nsutils/time.h`, `nsutils/base64.h`, `nsutils/unistd.h`
- `sys/time.h`, `sys/types.h`
- `shims/iconv.h`, `shims/zlib.h`, `shims/stat.h`
- `css/utils.h`
- `parserutils/charset/utf8.h`

### Access Paths (CodeWarrior)
All non-recursive. User paths:
- `{Project}::patrick:macsurf-source Folder:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:shims:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:parserutils:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:parserutils:charset:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:include:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:content:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:desktop:`
- `{Project}::patrick:macsurf-source Folder:browser:netsurf:utils:`

System paths:
- `{Compiler}:MacOS Support:Universal:Interfaces:CIncludes:`
- `{Compiler}:MacOS Support:MacHeaders:`
- `{Compiler}:MSL:MSL_C:MSL_Common:Include:`
- `{Compiler}:MSL:MSL_Extras:MSL_Common:Include:`
- `{Compiler}:MSL:MSL_C:MSL_MacOS:Include:`

### Linux Cross-Check
Use `gcc -fsyntax-only -std=c89 -pedantic -Dinline= -Ibrowser/netsurf/frontends/macos9/shims -Ibrowser/netsurf/frontends -Ibrowser/netsurf/include -Ibrowser/netsurf -include stdbool.h` to syntax-check frontend files on Linux before copying to Mac.

### Project File List (39 .c files)
Added to MacSurf.mcp — see macsurf-project.md for full list. `MacSurf.r` (the `'carb'` resource file) must also be in the project.

## Known Gotchas

- **`kInitOTForApplicationMask` and `kOTInvalidConfigurationRef` are not defined in CW8's OT headers.** Either `#define` them manually (`kInitOTForApplicationMask = 0x00000002`) or avoid them entirely by using the plain `InitOpenTransport()` path.
- **Including `<OpenTransport.h>` is safe** now that `kWindowStandardHandlerAttribute` has been removed from `CreateNewWindow`. An earlier crash that seemed like it was caused by including the header was actually the window-attribute bug manifesting later.
- **No `'carb'` resource → OTClientLib crash at a fixed address.** If the same instruction crashes every time somewhere inside OTClientLib, the cause is almost always that the binary is not a recognized Carbon fragment. Add `'carb'` before debugging anything else.
- **CW8 C89:** no `inline`, no `//` comments, no variadic macros, no forward enum declarations, no C99 designated initializers, no `for (int i...)`. All variables at the top of their enclosing block.
- **Mac CR line endings** are required for all `.c` / `.h` / `.r` files in the project. Convert with `sed 's/$/\r/' | tr -d '\n'` before packaging.
- **TextEdit (`TENew` / `TEDispose`) crashes with dsMemWZErr** on a fresh window because `GetWRefCon` returns garbage. Use direct `DrawString`/`DrawText` inside `BeginUpdate`/`EndUpdate` instead.
- **`kWindowStandardHandlerAttribute`** intercepts update events and leaves windows blank. Do not pass it to `CreateNewWindow`.