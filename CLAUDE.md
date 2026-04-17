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

- Development environment: Mac OS 9.1 on a Power Macintosh G3 Minitower (beige). All verified-working results come from this machine.
- Community compatibility target: Mac OS 9.2.2 on a Power Mac G4. Most-common active OS 9 setup today; not yet explicitly verified — open testing gap.
- Broader target range: Power Mac G3/G4, Mac OS 9.1-9.2.2, minimum 64MB RAM
- Compiler: CodeWarrior 8 (on-machine) or cross-compile GCC PPC from Linux
- No threading — OS 9 is cooperative multitasking, use WaitNextEvent loop
- No HTTPS in browser — all TLS handled by proxy
- JavaScript is handled in tiers, not banned:
  - **Base tier (G3 / 64MB floor):** Duktape 2.7.0 ES5 evaluator is linked into the base build and operational. Heavy / modern JS pages still go through the proxy render-and-flatten path.
  - **Enthusiast tier (G4 500+ / 256MB+):** same Duktape core as base tier, more ambitious inline-script scenarios enabled.
  - **Proxy tier:** headless Chromium/Playwright executes JS upstream and returns pre-rendered flat HTML. This is where real modern-site JS support lives.
- Carbon API for UI — works on OS 9 and early OS X

## Coding Conventions

- C for browser frontend (matches NetSurf codebase)
- Go for proxy
- Keep Mac Toolbox calls isolated in their own files (window.c, bitmap.c, font.c etc.)
- No external dependencies in proxy — stdlib only

## Carbon App Requirements

MacSurf is a Carbon CFM app running under CarbonLib on OS 9. For CarbonLib to fully engage, the binary MUST be identifiable as a Carbon fragment — otherwise `*InContext` calls crash at fixed addresses inside OTClientLib.

- **`'carb'` resource is mandatory.** Without it, CFM treats the binary as classic PEF, CarbonLib does not load as a dependency, and any `*InContext` OT call enters an uninitialized CarbonLib client context and crashes. This is the single most important requirement for a Carbon app on OS 9.
- **`MacSurf.rsrc`** contains the `'carb'` resource (zero-length, ID 0). It is a pre-compiled binary resource fork generated on Linux with a small Python script; CW8 links `.rsrc` files directly into the output resource fork with no Rez step. Must be listed in the CW8 project alongside the `.c` files.
- **`RegisterAppearanceClient()`** must be called at startup after `InitCursor()`, gated by a Gestalt check for Appearance Manager presence. Matches Classilla's `CBrowserApp` constructor pattern.
- **Skip** `InitGraf`/`InitFonts`/`InitWindows`/`InitMenus`/`TEInit`/`InitDialogs` under Carbon — Classilla explicitly skips them and so should MacSurf. Keep `InitCursor()` and `FlushEvents(everyEvent, 0)`.
- **No preemptive threads.** OS 9 is cooperative. Use `WaitNextEvent` for the UI event loop. OT yields happen through the notifier callback (see below).

## Open Transport Rules

MacSurf uses **plain (non-`InContext`) Open Transport calls**. This is verified against the Retro68 OT TCP demo and SSHeven, both of which run on real OS 9.2 hardware (external references — MacSurf itself develops and verifies on 9.1/G3).

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

- Do not rely on in-app JS for heavy/modern sites — Duktape is ES5-only and intended for small inline scripts. Real-site JS support is still the proxy's job (render-and-flatten).
- Do not enable tabs by default
- Do not use preemptive threads anywhere in the browser
- Do not add external dependencies to the proxy stdlib core. The render-and-flatten subsystem is an optional separate service (can use Chromium/Playwright); the base HTTP-proxy binary stays stdlib-only.
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
- `NO_IPV6 1`
- `TARGET_API_MAC_CARBON 1`
- `#include <MacTypes.h>` (first line — must stay first to prevent bool/true/false conflict)

`WITHOUT_DUKTAPE` is **no longer defined** — Duktape is linked into the base build. See [JavaScript Engine](#javascript-engine) below.

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

### Project File List (470 .c files)
Added to MacSurf.mcp:
- 12 frontend `.c` files
- 5 shim `.c` files
- 10 NetSurf core `.c` files (utils + content + desktop)
- 15 libparserutils
- 30 libhubbub
- 95 libdom
- 303 libcss

`MacSurf.rsrc` (pre-compiled binary `'carb'` resource, generated on Linux) must also be in the project — CW8 links `.rsrc` files directly into the output resource fork with no Rez step. The `*_stub.c` files exist on disk but are NOT in the project file list. See [docs/research/architecture-inventory.md](docs/research/architecture-inventory.md) for the full breakdown.

### Library Dependency Chain — COMPLETE

All five NetSurf core libraries are ported and C89-clean:

| Library | .c files | Status |
|---|---:|---|
| libwapcaplet | (via lwc_stub.c) | ✓ done at v0.1 |
| libparserutils | 15 | ✓ commit 8074a74 |
| libhubbub (HTML5 parser) | 30 | ✓ commit fd8d915 |
| libdom (DOM implementation) | 95 | ✓ commit 744232d |
| libcss (CSS parser + cascade) | 303 | ✓ commit 02628cf |
| **Total in MacSurf.mcp** | **443** | |

Combined LOC: ~125K. Stub footprint replaced: 3,688 lines (parserutils utf8.h + dom.h + libcss.h). All four port audits + execution reports live in [docs/research/](docs/research/):
- [parserutils-port.md](docs/research/parserutils-port.md)
- [libhubbub-port.md](docs/research/libhubbub-port.md)
- [libdom-port.md](docs/research/libdom-port.md)
- [libcss-port.md](docs/research/libcss-port.md)

**Next milestone — NetSurf core wiring (5 phases).** All five libraries are now ported and C89-clean (443 .c files in MacSurf.mcp), so the remaining work is glue between NetSurf core and the libraries. Full audit and sequencing in [docs/research/netsurf-core-wiring.md](docs/research/netsurf-core-wiring.md). Phases:

1. **HTTP fetcher rewrite** — `macos9_http_fetcher.c` implementing the real `fetcher_operation_table`, replacing the v0.1 standalone OT fetch path. Reuses the OT primitives from `macos9_fetch.c`. Delete `fetch_stub.c`.
2. **Content handler infrastructure** — add `content/content_factory.c` + ~9 utils/ helpers (corestrings, libdom, talloc, hashtable, idna, etc.) + ~4 desktop/ helpers (selection, scrollbar, textarea, system_colour). Cascading compile errors expected.
3. **CSS handler** — add 5 files from `content/handlers/css/`, convert designated initializers, delete `frontends/macos9/css/` stubs.
4. **HTML handler** — add 23 files from `content/handlers/html/`, convert ~9 designated initializers (including the `html_content_handler` vtable with 16+ function pointer fields), 1 for-scope decl in `layout_flex.c`, delete `frontends/macos9/html/` stubs, create `dom/bindings/hubbub/parser.h` wrapper header.
5. **End-to-end render** — implement `plot_text` / `plot_clip` / `plot_rectangle` in QuickDraw in `plotters.c`, wire `browser_window_create` to drive a real fetch through `hlcache_handle_retrieve`.

**Total scope:** ~44 new .c files in MacSurf.mcp (taking the project to ~487 files), ~800 lines of new frontend code, ~25 designated init conversions, 226 lines of stub deletion. **Image rendering deferred to v0.3** — `image_init()` is fully `#ifdef WITH_*` gated, so without `WITH_BMP`/`WITH_GIF`/etc. it's a no-op and saves 28 files / 5.4K LOC of work this milestone.

**Most likely bottleneck:** the talloc question. NetSurf's `utils/talloc.c` is a Samba-derived hierarchical allocator with POSIX-y patterns; if it doesn't compile under CW8 it needs its own port pass before HTML can land. Documented in §13 open question 3 of the wiring audit.

### Library port audit checklist

When auditing a new C99 library for CW8 / strict C89, grep for:
- `inline` keyword
- `//` line comments (start-of-line AND trailing — but EXCLUDE URLs in `/* */` block comments, especially `http://www.opensource.org/licenses/...`)
- C99 designated initializers (`^\s*\.\w+\s*=`) — and **count instances per file**, not just file count. format_list_style.c had 47 in one file.
- For-scope declarations: integer types AND **pointer-type variants** (`for (TYPE *NAME = ...)`, `for (const TYPE *NAME = ...)`). The libcss audit missed pointer-type for-scope and undercounted by 10 sites.
- `restrict` keyword
- Compound literals
- `__VA_ARGS__` variadic macros
- `long long`
- Variable-length arrays
- Flexible array members
- Forward enum declarations
- `__attribute__` / `__builtin_*`
- `snprintf` / `vsnprintf`
- `%zu` / `%zd` printf formats
- `<iconv.h>`, `<errno.h>`, `<strings.h>`, `<sys/types.h>` and other POSIX
- **GNU union casts** — `(union_type)0` or `(typedef_name)expr` where the typedef resolves to a union. Standard C89 forbids casting to union types. The libcss audit missed 5 sites of `(css_fixed_or_calc)0`.
- **Union initializers using designated syntax** — `{.field = value}` for a typedef'd union looks identical to a struct designated init in grep output. C89 union initializers must use `{value}` (positional, first member only).
- Build-time codegen (`gperf`, perl scripts, `.inc` files included from `.c` files)
- Existing MacSurf stubs in `frontends/macos9/<libname>/` that will conflict with the real headers

## JavaScript Engine

- Duktape 2.7.0 is fully linked and operational in the base build.
- ES5 evaluator confirmed working — stress tests pass including closures, prototypes, regex, JSON, promises, recursion, matrix multiply, Mandelbrot.
- `js_newheap` / `js_destroyheap` / `js_exec` lifecycle working.
- `WITHOUT_DUKTAPE` has been removed from `macsurf_prefix.h`.
- Duktape source files live in [browser/libduktape/](browser/libduktape/).
- `duk_config.h` is hand-crafted for Mac OS 9 PPC CW8: `DUK_USE_BYTEORDER=3`, `DUK_USE_PACKED_TVAL`, `DUK_USE_ALIGN_BY=8`, `DUK_USE_NATIVE_CALL_RECLIMIT=128`.
- JS glue files live in [browser/netsurf/frontends/macos9/javascript/](browser/netsurf/frontends/macos9/javascript/).

## Rendering Pipeline (v0.3 in progress)

- HTTP fetcher registered for `http:` and `https:` schemes via OT proxy at `116.202.231.103:8765`.
- Current blocker: `htmlc->base.active` stays at 2 — the stylesheet loading chain is not completing.
- Root causes identified:
  - Resource fetcher serves empty CSS.
  - Scheduler pump is not called frequently enough.
  - `no_backing_store` returns `NSERROR_OK` on store operations, corrupting llcache state.
- Fix plan documented in [docs/macsurf-v03-render-plan.md](docs/macsurf-v03-render-plan.md).
- v0.3 target: real HTML rendering with styled text, colors, fonts, and layout via the full NetSurf pipeline.

## Build State

- MacSurf v0.2 is stable — loads plain text pages, JS executes, OT networking works.
- CW8 project file is [browser/netsurf/frontends/macos9/MacSurf.mcp](browser/netsurf/frontends/macos9/MacSurf.mcp).
- Flat-folder build approach — all `.c` files in one folder, one search path.
- Remove Object Code is required before every rebuild after file changes.
- MacsBug is installed on the G4 for pipeline debugging — `MS_LOG` checkpoints are active throughout the pipeline.

## Known Issues

- `no_backing_store.c` must return `NSERROR_NOT_IMPLEMENTED` from both store and fetch — currently returns `NSERROR_OK`, which corrupts llcache.
- Resource fetcher must serve real content for `resource:default.css`, `resource:internal.css`, and `resource:quirks.css`.
- Scheduler pump must call `fetch_poll()` on every event-loop pass when fetches are pending, not only when Mac events are present.

## Docs

- [docs/macsurf-architecture.md](docs/macsurf-architecture.md) — Full platform architecture: rendering modes, proxy services, template system, milestone plan
- [docs/research/architecture-inventory.md](docs/research/architecture-inventory.md) — Snapshot of what currently exists in the repo and on the proxy host (no decisions, just facts)
- [docs/status.md](docs/status.md) — Project status, milestones, test environment
- [docs/codewarrior-setup.md](docs/codewarrior-setup.md) — How to install CodeWarrior 8 and build on a real Power Mac
- [docs/deploying-proxy.md](docs/deploying-proxy.md) — How to deploy the Go proxy

## Known Gotchas

- **`kInitOTForApplicationMask` and `kOTInvalidConfigurationRef` are not defined in CW8's OT headers.** Either `#define` them manually (`kInitOTForApplicationMask = 0x00000002`) or avoid them entirely by using the plain `InitOpenTransport()` path.
- **Including `<OpenTransport.h>` is safe** now that `kWindowStandardHandlerAttribute` has been removed from `CreateNewWindow`. An earlier crash that seemed like it was caused by including the header was actually the window-attribute bug manifesting later.
- **No `'carb'` resource → OTClientLib crash at a fixed address.** If the same instruction crashes every time somewhere inside OTClientLib, the cause is almost always that the binary is not a recognized Carbon fragment. Add `'carb'` before debugging anything else.
- **CW8 C89:** no `inline`, no `//` comments, no variadic macros, no forward enum declarations, no C99 designated initializers, no `for (int i...)`. All variables at the top of their enclosing block.
- **Mac CR line endings** are required for all `.c` / `.h` / `.r` files in the project. Convert with `sed 's/$/\r/' | tr -d '\n'` before packaging.
- **TextEdit (`TENew` / `TEDispose`) crashes with dsMemWZErr** on a fresh window because `GetWRefCon` returns garbage. Use direct `DrawString`/`DrawText` inside `BeginUpdate`/`EndUpdate` instead.
- **`kWindowStandardHandlerAttribute`** intercepts update events and leaves windows blank. Do not pass it to `CreateNewWindow`.