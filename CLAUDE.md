DO NOT BLAME STALE FILES FOR ANY REPEATING ISSUES. CHECK FOR OTHER POSSIBILITIES AND NEVER DISREGARD THE USER'S STATEMENTS ABOUT BUILDING THE FILES CORRECTLY.Warning : variable 'have_delcount' is not initialized before being used
duktape.c line 1106   duk_bool_t have_delcount;

Warning : variable 'rc' is not initialized before being used
duktape.c line 1159   duk_bool_t rc;

Warning : variable 'cx' is not initialized before being used
duktape.c line 141   duk_small_int_t cx, cy, sx;

Warning : variable 'sx' is not initialized before being used
duktape.c line 141   duk_small_int_t cx, cy, sx;

Warning : variable 'pc_before_expr' is not initialized before being used
duktape.c line 6059   duk_int_t pc_before_expr;

Warning : variable 'pc_after_expr' is not initialized before being used
duktape.c line 6059   duk_int_t pc_after_expr;

Warning : variable 'len_value' is not initialized before being used
duktape.c line 497   duk_uint_t len_value;

Link Error   : undefined: 'dom_document_create_element' (code)
Referenced from 'macsurf_createElement' in macsurf_js_dom.c

Link Error   : undefined: 'dom_node_unref' (code)
Referenced from 'element_finalizer' in macsurf_js_dom.c

Link Error   : undefined: 'dom_node_append_child' (code)
Referenced from 'macsurf_appendChild' in macsurf_js_dom.c

Link Error   : undefined: 'dom_element_set_attribute' (code)
Referenced from 'macsurf_setAttribute' in macsurf_js_dom.c

Link Error   : undefined: 'dom_element_get_tag_name' (code)
Referenced from 'macsurf_getTagName' in macsurf_js_dom.c

Link Error   : undefined: 'dom_node_ref' (code)
Referenced from 'macsurf_push_element' in macsurf_js_dom.c

Link Error   : undefined: 'dom_document_get_element_by_id' (code)
Referenced from 'macsurf_getElementById' in macsurf_js_dom.c

Link Error   : undefined: 'dom_string_unref' (code)
Referenced from 'macsurf_createElement' in macsurf_js_dom.c
Referenced from 'macsurf_setAttribute' in macsurf_js_dom.c
Referenced from 'macsurf_getAttribute' in macsurf_js_dom.c
Referenced from 'macsurf_getTagName' in macsurf_js_dom.c
Referenced from 'macsurf_getElementById' in macsurf_js_dom.c
Referenced from 'macsurf_js_dispatch_event' in macsurf_js_dom.c

Link Error   : undefined: 'dom_element_get_attribute' (code)
Referenced from 'macsurf_getAttribute' in macsurf_js_dom.c
Referenced from 'macsurf_js_dispatch_event' in macsurf_js_dom.c



# MacSurf

A lightweight web browser for Mac OS 9 PowerPC, built on the NetSurf engine, paired with a simple TLS proxy.

## CLAUDE.md Maintenance

This file must be kept current. It falls out of date fast when not actively maintained, and stale context causes agents to repeat solved problems. Update as part of every round that changes project state — when a blocker is resolved, when a new subsystem lands, when the architecture shifts. The goal is that any new agent reading CLAUDE.md at the start of a session has an accurate picture of where the project actually stands. Detailed update protocol at the bottom of this file.

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

### Adding new .c files
When a change introduces a new `.c` file, mention it plainly so the user can add it to the project. **Do NOT edit `MacSurf.mcp` and do NOT include it in fix zips** — the user maintains the project file list on the Mac side through the CW8 IDE, and a Linux-edited `.mcp` will clobber their local changes. Just list the new filename(s) in the handoff and let the user add them.

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

## Browser Chrome

- Pixel-based scrolling operates on `content_get_height()`, not the v0.1 text-line model.
- Address bar routes through `browser_window_navigate` with `nsurl_create` URL normalization (prepends `http://` if no scheme).
- Back, Forward, Reload, Home buttons wired to real NetSurf navigation APIs.
- Status bar displays NetSurf status messages and hovered link URLs.
- Title bar auto-updates via `gui_window_set_title` from page content.
- Window resize triggers `browser_window_schedule_reformat` via a deferred flag pattern to prevent re-entrant layout.
- `MACSURF_HOME_URL` defined in `macsurf_config.h`.
- v0.1 fallback path (`strip_html` + direct `DrawText`) has been removed. Full NetSurf pipeline is the only rendering path.

## Mouse Wheel / Input Devices

- **No Carbon wheel handler.** `kEventMouseWheelMoved` is **not available in CarbonLib** on OS 9 — Apple's own `CarbonEvents.h` marks the event class as `Mac OS X: 10.0+ in Carbon.framework; CarbonLib: not available`. fixes134 attempted to install a handler and the Mac crashed with illegal-instruction at `19DBDEB8` because CarbonLib's dispatcher destabilizes when asked about events whose class was never back-ported. Root-caused and disabled in fixes140. See [browser/netsurf/frontends/macos9/macos9_wheel.c](browser/netsurf/frontends/macos9/macos9_wheel.c) — `macos9_wheel_install()` is retained as a visible no-op for ABI stability (Mac-side main.c may still reference it).
- **fixes141 — interim defensive hardening.** Even with the Carbon wheel handler disabled, spinning the wheel under MacSurf still dropped into MacsBug with `Undefined A-Trap at 1BDC54E0` (no procedure name — execution in garbage memory). fixes141 narrowed the `WaitNextEvent` mask to an explicit whitelist of classic event kinds and added a matching whitelist guard at the top of `macos9_dispatch_event` so any unknown event class is silently dropped before touching any Toolbox or browser-core code. This is hardening, not diagnosis — the underlying crash is likely inside CarbonLib or USB Overdrive's trap patches and cannot be debugged further without capturing a real MacsBug stack, which requires an ADB keyboard the user does not currently have. **Proper wheel-crash diagnosis deferred until ADB hardware is available.**
- **USB Overdrive — current recommendation: "Do Nothing" on the Scroll Wheel action** until the wheel-crash root cause is understood. Users should configure USB Overdrive's Scroll Wheel setting to "Do Nothing" (or not install a wheel binding at all) when MacSurf is frontmost. Scrolling works via scroll bar, keyboard arrows, Page Up/Down, and Home/End. The previous recommendation (Up/Down arrow keys) is valid in principle — it flows through `macos9_handle_key_down` — but may still trigger the underlying crash if USB Overdrive's trap patches touch state before the synthesized key event reaches us. See [docs/usb-overdrive.md](docs/usb-overdrive.md).
- **Complete scroll-input set on OS 9 without the wheel:** scroll-bar drag, keyboard arrows, Page Up/Down, Home/End. All keyboard-sourced paths are tested and working. Carbon-native wheel events are architecturally out of reach on this platform regardless of the fixes141 defensive work.

## Rendering Pipeline (v0.3 — native CSS)

- HTTP fetcher registered for `http:` and `https:` schemes via OT proxy at `116.202.231.103:8765`.
- Resource fetcher serves real CSS content for `resource:default.css`, `resource:internal.css`, `resource:quirks.css` (`macos9_fetcher_stubs.c`).
- `no_backing_store.c` returns `NSERROR_NOT_IMPLEMENTED` from store and fetch.
- Event-loop sleep shortens to 1 tick while any fetcher is active (`macos9_fetching || macos9_stub_fetcher_active() || macos9_http_fetcher_active()`) so NetSurf's fetcher ring progresses via `fetch_send_callback` continuations every pass. There is **no** explicit `fetch_poll()` call.
- **Full NetSurf pipeline executes: fetch → parse → CSS cascade with native var() resolution → layout → plot.**
- **Real HTML rendering with styled text, colours, fonts, layout all working natively.** MacTrove (Drupal 11 site) loads with body background, card chrome, link colours, and theme fonts resolving correctly from CSS custom properties. Verified signal: title bar shows `cp res OK`.
- **Architectural foundation:** [docs/research/state-survey-2026-04-18.md](docs/research/state-survey-2026-04-18.md) and [state-survey-2026-04-19.md](docs/research/state-survey-2026-04-19.md). The 2026-04-19 survey in particular (§A7) explicitly scoped three paths for var() support — native libcss, proxy preprocessor, browser preprocessor — and chose native. Without that scoping, the fast-looking proxy shortcut would have blocked the real fix. Treat both surveys as load-bearing architectural refs for any future CSS-layer work.
- Screenshot canonical location: `screenshots/v0.3-mactrove-fixes139.png` (user-saved from the 2026-04-20 session).
- Carbon partition bumped to 16 MB preferred / 8 MB minimum to accommodate libcss allocation footprint on real pages (CSS_NOMEM blocker long resolved; see Gotchas).

### Current blockers — feature gaps, not pipeline bugs

- **`gap` / `row-gap` — parsed, single-value fidelity only (fixes148).** `gap: N` and `row-gap: N` both emit column-gap bytecode; `layout_flex.c` reads `css_computed_column_gap` and applies it as main-axis gap between items and cross-axis gap between wrapped lines. Two-value `gap: A B` loses one value (column-gap = B wins). Full-fidelity row-gap (independent storage, bit-packing extension) deferred. MacTrove usage audit: 74 single-value + 2 two-value, so the 97% case is covered.
- **Flex `justify-content` / `align-content` / `order` computed by libcss but unread by `layout_flex.c`** — layout ignores them. Queued fixes149.
- **`border-radius`, `box-shadow`, gradients, transforms not parsed** — cosmetic loss. Queued fixes150 (border-radius first via QuickDraw `PaintRoundRect`).
- **Image content handlers not linked** — every `<img>` renders as placeholder box. Queued fixes151.
- **URL field input fails on the initial window, works on File → New Window** — queued for a dedicated probe round. Hypothesis in 2026-04-18 survey: content redraw during `browser_window_create` overdraws the URL rect visually while TextEdit is still functional internally.

## Native CSS Custom Properties

Shipped incrementally across fixes133-139. Native libcss implementation, not a proxy preprocessor. Per-document scope, tokens preserved through cascade, resolved at selection time (option c from the 2026-04-19 architecture decision).

- **Scope.** Custom property definitions captured from any rule with `--name: value` syntax (simplified from the full CSS spec which restricts to `:root`, `html`, `*` — treated as globally scoped within document for this implementation, which matches observed behaviour of every real stylesheet we've looked at).
- **Capture.** Each stylesheet keeps a `custom_properties` linked list of `css_cp_entry { lwc_string *name; css_cp_token *tokens; uint32_t n_tokens; next; }`. Populated from `handleDeclaration` when the first token is an `IDENT` whose idata starts with `--`. See [browser/libcss/src/parse/language.c](browser/libcss/src/parse/language.c) and [browser/libcss/src/parse/custom_properties.c](browser/libcss/src/parse/custom_properties.c).
- **Resolution.** `var(--name)` is resolved at cascade time via token substitution before the property-specific parsers run. The select-ctx-wide aggregate table combines all sheets' tables with last-write-wins over source order, matching author cascade. Nested `var()` is resolved recursively with a depth-10 cap to prevent infinite recursion on circular references.
- **Fallbacks.** `var(--name, fallback)` supported. Fallback can itself contain `var()`.
- **!important.** Preserved through substitution.
- **Keystone fix (fixes139).** `lex.c` `CDCOrIdentOrFunctionOrNPD` — when `--` is followed by `startNMStart(c)`, append and continue into `IdentOrFunction` rather than emitting `CHAR('-') + IDENT('-foo')`. Without this, libcss's CSS 2.1-era lexer splits `--foo` into two tokens that the declaration parser rejects. fixes133's capture logic was sound; it never had a chance to run until fixes139 landed the lexer branch. See [browser/libcss/src/lex/lex.c](browser/libcss/src/lex/lex.c).

## Native CSS3 Strategy

MacSurf handles modern CSS natively in libcss and the layout engine rather than preprocessing via the proxy. This preserves the "real web browser running natively on Mac OS 9" value proposition. The proxy strips TLS and optionally renders-and-flattens JavaScript-heavy sites; it does **not** preprocess CSS for browser limitations.

Native support landed or in progress:

- CSS custom properties (`var()`) — **fixes133-139, shipped.**
- `gap` / `row-gap` — **fixes148, shipped with single-value fidelity (97% of MacTrove uses); two-value `gap: A B` degrades to column-gap=B.** Full-fidelity row-gap requires the bit-packing round deferred earlier.
- Flex alignment (`justify-content`, `align-content`, `order`) reads — queued fixes149.
- `border-radius` via QuickDraw `PaintRoundRect` / `FrameRoundRect` — queued fixes143.
- Image content handlers (GIF/PNG/JPEG) — queued fixes144.

Features that remain unsupported and degrade gracefully to block layout or flat rendering: CSS Grid (collapses to block), `box-shadow`, `transform`, `transition`, `animation`, gradients, `clip-path`, `mask`. These are cosmetic in most cases and their absence does not prevent page comprehension.

## Build State

- MacSurf v0.3 renders real live web pages on G3 hardware with native CSS custom property support.
- First confirmed page: MacTrove (`http://mac.mp.ls/`), 2026-04-19, via the full NetSurf pipeline.
- v0.2 baseline (plain text, JS, OT networking) remains stable.
- CW8 project file is [browser/netsurf/frontends/macos9/MacSurf.mcp](browser/netsurf/frontends/macos9/MacSurf.mcp).
- Carbon partition: **16 MB preferred / 8 MB minimum** (`MWProject_PPC_size` / `MWProject_PPC_minsize`). Anything smaller starves libcss and triggers CSS_NOMEM mid-cascade on real pages.
- Flat-folder build approach — all `.c` files in one folder, one search path.
- Remove Object Code is required before every rebuild after file changes.
- MacsBug is installed on the G4 for pipeline debugging — `MS_LOG` checkpoints are active throughout the pipeline.
- Last shipped fix: **fixes178** — Critical fix for property index shift: Moved `BORDER_RADIUS` and `BOX_SHADOW` to the end of the `propstrings.h` list. Previously, they were inserted alphabetically, which shifted the indices of all subsequent properties (like `display`, `position`, `width`), causing the CSS engine to use wrong handlers and break the entire layout (unstyled rendering). Also removed a problematic `.*` precision log that was failing on OS 9.
- Predecessor: **fixes177** — Fixed illegal implicit conversion in `p_box_shadow.c` by changing `color_type` from `uint8_t` to `uint16_t` to match the `css__parse_colour_specifier` declaration.
- Predecessor: **fixes176** — CodeWarrior filename conflict fix: Renamed new property files to `p_border_radius.c`, `s_border_radius.c`, `p_box_shadow.c`, and `s_box_shadow.c`. This prevents object-file collisions in CodeWarrior's flattened build folders and resolves the missing descriptor link errors.
- Predecessor: **fixes175** — Deep CSS3 expansion: Implemented `box-shadow` parser and selection handler. Updated `libcss` Makefiles and `redraw_border.c` C89 compliance.
- Predecessor: **fixes172** — Native `border-radius` support. Added full pipeline for `border-radius` from `libcss` parsing to QuickDraw `FrameRoundRect` / `PaintRoundRect`. This provides hardware-accelerated rounded corners natively on OS 9 without proxy preprocessing.
- Predecessor: **fixes171** — Text rendering fix: Added `macos9_utf8_to_macroman` conversion in `macos9_font.c`. Because NetSurf passes UTF-8 to the frontend, strings with multi-byte characters (like curly quotes or the `•` bullet point in `simple.html` which showed as `,A¢`) were being passed raw to `TextWidth` and `DrawText`. This caused `TextWidth` to return inaccurate lengths (measuring 3 MacRoman characters instead of 1), leading to text overlap and broken inline box models. Added conversion before measurement and drawing.
- Predecessor: **fixes170** — Font matching by name ("Geneva", "Monaco", "Chicago", "Charcoal") and actual QuickDraw `TextWidth()` measurement in layout. Plumbed `initial_win` as a global so `macos9_font.c` can access a valid `GrafPort` during NetSurf's asynchronous layout callbacks. Added `PenSize` support to plotter table to respect `stroke_width` (corrects borders and `<hr>` rendering). Exported `strcasecmp` shim for font identification.
- Predecessor: **fixes167** — Back out `TESetSelect(0, 32767)` in `TEClick` path; it was breaking typing in the URL field.
- Older: **fixes160** — two post-fixes159 scroll polish items on hardware-confirmed report: (1) call `macos9_window_update_extents` from the `GW_EVENT_STOP_THROBBER` handler so scroll bars pick up the final rendered height after load completion (initial `UPDATE_EXTENT` event fires before late CSS-resolve / font / flex reflow passes finish, leaving `content_height` stale and scroll bars disabled until a manual window resize forces `update_scrollbars` via the resize path); (2) bump `MACOS9_KEY_SCROLL_STEP` from 16 to 48 so an arrow click or keyboard arrow moves a more useful distance on real pages — 16px was matching Mac classic line-height but feels "one line at a time" on web content.
- Predecessor: **fixes159** — drop the Appearance live-tracking scroll bar CDEF (`kControlScrollBarLiveProc = 386`) for the non-live variant (`kControlScrollBarProc = 384`). On G3/G4 hardware, clicking a scroll bar crashed into MacsBug with IDENT pointing at an Appearance Manager internal (symbol prefix `hD`); SheepShaver never reproduced it. fixes147 had already removed the UPP macro override and switched `TrackControl` to `NULL` action, but the crash persisted, leaving proc 386's live-track CDEF itself as the remaining suspect on the drop path from `TrackControl`. Proc 384 is the non-live Appearance scroll bar: the CDEF handles thumb visual feedback on its own without the per-frame app-callback cadence that live-tracking uses, so whatever in the live path corrupts or races on real hardware never runs. Trade-off: during drag, content does NOT scroll live — thumb moves visually, then content snaps to the final position on mouseup. This is still the existing `GetControlValue`-on-return path from fixes147, so only the CDEF procID changed. Arrow / page clicks scroll by one step per click exactly as before. **Hardware-confirmed:** scroll bar click no longer crashes.
- Predecessor: **fixes152** — hotfix to actually wire `macsurf_debug_log_init()` into `main()`. **fixes149 shipped the log infrastructure but never called the init** — three rounds (fixes149, fixes150, fixes151) of instrumentation silently discarded because `g_log_open` stayed 0 and every `macsurf_debug_log_write*` short-circuited at the first-line gate. fixes152 also adds title-bar fallback messages on each init-failure branch (FindFolder / FSpCreate / FSMakeFSSpec / FSpOpenDF) so a second missed-init regression would surface in 30 seconds rather than three rounds. See "Regression Audit Checklist" below.
- Predecessor: **fixes151** — `SelectWindow` after `ShowWindow` in `macos9_window_create` + `Draw1Control` on all four toolbar buttons in `update_button_states` + `MACSURF_HOME_URL` switched to `http://mac.mp.ls/simple.html`. Research-run conclusion that `ShowWindow` alone doesn't generate activateEvt under Carbon OS 9. Instrumentation planned to verify — silently didn't because fixes149's log channel was dead (see fixes152).
- Earlier: **fixes150** — `url_field_active=true` at window creation, `TEActivate` unconditional in `macos9_window_create`, `Draw1Control` in `macos9_window_scroll_to`, `macos9_handle_activate` propagates window activation to all controls via `HiliteControl`/`Draw1Control`. Targeted two real bugs (URL bar unresponsive, scroll bars grey). Verification pending because log channel was dead.
- Earlier: **fixes149** — file-backed diagnostic channel (`MacSurf Debug.log` on Desktop), heavy scroll-bar-click and URL-field instrumentation, defensive `SetPortWindowPort` at scroll-handler entry. **Shipped broken:** `macsurf_debug_log_init()` was never called from `main()`. Fixed in fixes152.
- Earlier: **fixes148** — `gap` and `row-gap` native parsing + `layout_flex.c` consumption (single-value fidelity). See "Native CSS3 Strategy" above and the "`row-gap` shares storage" entry in Known Gotchas.
- Older predecessor: **fixes147** — two commits in one zip:
  - **Commit A (scroll-bar click crash — partial, hypothesis addressed, real-hardware result pending):** Clicking anywhere in a scroll bar crashed on the G3 at `PowerPC illegal instruction at 00000008, LR=00000004` with CurApName `CodeWarrio...`. Hypothesis: the UPP macro override in [window.c](browser/netsurf/frontends/macos9/window.c) — `#define NewControlActionUPP(proc) ((ControlActionUPP)(proc))` — was passing a raw PPC function pointer where CarbonLib's `TrackControl` expects a MixedMode-compatible routine descriptor. Fix: drop the action UPP entirely. `TrackControl(ctrl, pt, NULL)` — no UPP dispatch. CDEF live-tracks the thumb; `GetControlValue` on return holds the final drag position. Arrow/page clicks scroll by one step per click based on the `ControlPartCode` the caller captured from `FindControl`. Trade-off: no auto-repeat on arrow-hold (keyboard arrows still repeat). **Verified in SheepShaver** (no crash on any scroll-bar click — vertical track, up/down arrows, page regions, thumb drag). **Real-hardware result still pending** — if G3/G4 still crashes, the UPP hypothesis was wrong and this joins the wheel crash in the "hardware-specific, needs MacsBug" bucket. See the new "UPP macro override on CarbonLib" entry in Known Gotchas regardless — it was a latent bug whether or not it was the crash root cause.
  - **Commit B (docs + probe strip):** Preserves the 2026-04-19 survey and MacTrove CSS samples. Strips the stale `sbar h=... vh=... max=...` one-shot probe from `macos9_window_update_extents` (its diagnostic purpose is closed — extents confirmed flowing correctly from `browser_window_get_extents`).
  - Older predecessors: fixes146 (shutdown ordering + update-handler hardening), fixes145 (probe removal), fixes144 (wne/disp probes), fixes143 (distinct-kinds probe), fixes142 (scroll-bar hardening + one-shot sbar probe), fixes141 (event-class whitelist), fixes140 (wheel handler disable). **Next fix ships as fixes171** — numbering is monotonic per user convention; always confirm the number with the user before shipping.

**fixes146 outcomes (hardware-tested during fixes147 prep):**

- **Quit works cleanly** — Commit A **confirmed on hardware**, shutdown bug closed.
- **MacTrove render stable through fixes146** — var() resolution intact, Platinum theme rendering correctly, window resize works on real content.
- **Wheel still crashes (separate from scroll bar).** Different signature from fixes147's crash. Remaining Linux-source hypothesis space exhausted; progress gated on ADB-keyboard MacsBug `wh` capture. See "Wheel crash diagnostic exhaustion" below.

### Next work queue

- **Read the fixes149 log file first.** Tester runs the seven-criterion chrome gauntlet from the fixes149 brief; `MacSurf Debug.log` on Desktop is the deliverable. Scroll-bar crash hypothesis tree narrows as follows from log output:
  - `sb_handler` logged but `sb_ctrl_state` missing → ControlRef is stale (dangling handle); fix is lifetime-tracking the control pointers (fixes146 already nulls them on destroy; possible next step is version-token validation).
  - `sb_call_track` logged but `sb_post_track` missing → `TrackControl` itself crashes on the live-tracking CDEF. Likely Appearance Manager state corruption; next step is disabling the live CDEF (drop procID 386 → 16 classic scroll bar) as a workaround.
  - All pre-TrackControl logs AND `sb_post_track` present but no `sb_dispatch` → post-TrackControl branch dispatch crashes (unlikely given the code is pure int math, but possible if GetControlValue returns garbage).
  - Clean run with no crash on real hardware → fixes147's drop-the-UPP fix was correct all along, just needed the defensive SetPortWindowPort in fixes149 to close it fully.
- **fixes150 — flex alignment reads in `layout_flex.c`.** libcss computes `justify-content` / `align-content` / `order`; layout ignores them. Follow the `lh__box_align_self` pattern. (column-gap is now consumed as of fixes148.) Ship after fixes149 closes the chrome verification loop.
- **fixes151 — `border-radius` via `PaintRoundRect` / `FrameRoundRect`.** 30 uses in MacTrove. Plumb `corner_radius` through `plot_style_t`.
- **fixes152 — image content handlers (GIF/PNG/JPEG).** Every `<img>` becomes a real image. Bottleneck: talloc on CW8.
- **Full-fidelity `row-gap` (deferred).** Split row-gap from column-gap storage — new `CSS_PROP_ROW_GAP` enum entry, new field in `css_computed_style_i`, new bit slot in `autogenerated_computed.h` (word 14 has free bits), new propset/propget macros, new parse + select files, `css_computed_row_gap` accessor, wire `layout_flex.c` to read both independently. Currently fixes148 parses both properties into `column-gap` storage; two-value `gap: A B` loses the first value. Only worth doing if real-world pages exercise the two-value form enough to notice.
- **Wheel crash diagnostic exhaustion — Linux-source audit is DONE, further progress requires MacsBug access.** Evidence summary (do not redo this audit from Linux):
  - `sbar h=<real> vh=<real> max=<real>` probe fires cleanly after MacTrove loads — extents flow through `browser_window_get_extents` correctly.
  - `disp w=6` (updateEvt) latches on wheel spin — dispatcher is reached, crash is downstream of `macos9_dispatch_event` entry.
  - Crash at `1A23829E` with `A67C1A23 not.w #6691` disassembly — 68k-looking data executed as code, classic "called through a function pointer that wasn't a routine descriptor."
  - `macos9_plotters` table fully populated (9 function pointers, 3 NULL, 1 bool); core guards the NULL slots.
  - No stray `DisposeControl` anywhere; controls disposed only implicitly via `DisposeWindow` (close box or shutdown).
  - **Updated by fixes147:** the claim that "no UPP is live during wheel spin" was true but the scroll_action UPP itself was structurally malformed (raw function pointer instead of RoutineDescriptor). That bug was real and caused the separate scroll-bar click crash, not the wheel crash. Wheel crash remains unexplained.
  - Every struct is `calloc`'d; every Pascal-string write is bounded; every Str255 local is written by set_pstring before Toolbox call.
  - fixes141 narrowed `WaitNextEvent` mask + whitelist guard at dispatch; fixes142 added `SetPortWindowPort` + `Draw1Control` to scroll-bar updates; fixes145 removed probe-SetWTitle Memory Manager pressure; fixes146 adds quitting-flag and null-before-DisposeWindow hardening; fixes147 removes the scroll_action UPP entirely (which was structurally malformed but probably not the wheel-crash path since wheel spin doesn't route through TrackControl). **None of these hardenings conclusively fixed the wheel crash — if it persists, the remaining hypothesis (Control Manager state corrupted somewhere we can't inspect) can only be confirmed with a MacsBug `wh` stack capture.** Next step: get ADB keyboard, reproduce, `wh`, `sc`, `ip`, `dm sp` — then we have a caller chain to fix.
- **URL field on initial window — dedicated probe round.** Add a one-shot probe in `plot_clip` / `plot_rectangle` logging coordinates that intersect `gw->url_rect` to confirm the content-redraw-overdraws-URL hypothesis from the 2026-04-18 survey §1.

## Regression Audit Checklist

**New in fixes152 after the fixes149 missed-init regression.**

Any new subsystem shipped as part of a fix round MUST, before the round is closed, satisfy all three:

1. **Init call wired.** Grep the entrypoint (`main.c main()` for MacSurf) for the init function name. `grep -c macsurf_foo_init main.c` must return a non-zero count. If zero, the subsystem is linked but dead.
2. **Smoke test confirming it runs.** Either a SheepShaver smoke launch (boot, relaunch, confirm the subsystem's externally-visible artefact — file, title-bar message, menu item — is present) or a hardware cycle. "Linux syntax check passes" is NOT a smoke test; syntax passes on code that is never executed.
3. **Dependency documented.** Add a one-line entry under this section's table for new infrastructure:

| Subsystem | Init function | Init call site | Externally-visible artefact at startup |
|---|---|---|---|
| File-backed diagnostic log | `macsurf_debug_log_init` | `main.c main()` after `FlushEvents` | `MacSurf Debug.log` on Desktop with `=== MacSurf startup ===` entry |
| Open Transport | `InitOpenTransportInContext` | `main.c main()` after log init | `ot_initialized = true` (internal) |
| NetSurf core | `netsurf_init` | `main.c main()` after OT init | Window shows with content pipeline live |
| Carbon Appearance | `RegisterAppearanceClient` | `main.c main()` after `InitCursor` | Controls render with platinum theme, not classic |

**Why this checklist exists.** fixes149 through fixes151 accumulated ~20 instrumentation lines across window.c / main.c / the handle functions. None of them wrote anything to the log file because `macsurf_debug_log_init` was never called. The bug was invisible because:
- `macsurf_debug_log_write` short-circuits silently when `g_log_open == 0` — no stderr, no toolbox call, no stdout.
- MacSurf Debug.log on the Desktop never appeared — but its absence wasn't flagged by any build step.
- fixes149/150/151 all "landed" per git and per user test cycles, but produced zero log output.
- A SheepShaver smoke test ("launch, quit, ls Desktop") would have caught it in 30 seconds of the first ship.

**When reviewing / shipping a fix round:** if the round adds or touches a subsystem in the table above, verify the init path still fires. If the round adds a new subsystem, add an entry. Missed-init is the exact class of regression this table catches.

## File-Backed Diagnostic Channel

Shipped in fixes149. Writes one CR-terminated line per log call to
**`MacSurf Debug.log`** on the Desktop, flushing after every write
(`FlushVol` + `SetFPos` pair) so the file survives illegal-instruction
crashes, frozen Macs, and forced restarts. This is the **primary
post-crash back trace channel for MacSurf** on hardware we can't
attach MacsBug to.

- API: [browser/netsurf/frontends/macos9/macsurf_debug_log.h](browser/netsurf/frontends/macos9/macsurf_debug_log.h). `macsurf_debug_log_init()` at startup, `_close()` at shutdown, `_write(str)` for literal strings, `_writef(fmt, ...)` for minimal printf (`%d`, `%ld`, `%p`, `%s`, `%%`).
- `MS_LOG(msg)` now dual-channels: title bar (live feedback) **and** log file (durable record). File write comes first so a SetWTitle-adjacent crash still leaves the log entry on disk.
- `_writef` uses a hand-rolled formatter, NOT MSL's `vsnprintf` (unreliable on CW8 Carbon MSL). Supports only the format specifiers used by MacSurf instrumentation. Output is hard-capped at 255 bytes.
- Log file location: Desktop (via `FindFolder(kOnSystemDisk, kDesktopFolderType, ...)`). If FindFolder fails the log is silently inert — init does not crash, subsequent calls no-op.
- Reading the log: open `MacSurf Debug.log` in SimpleText. Each line is one log call. Crash forensics = "the last N lines before the log ends show the code path that was executing when the Mac died."
- **Release builds (`MACSURF_RELEASE` set) compile the channel to empty stubs** — symbols stay exported for link compatibility, but no file operations happen.
- **Gotcha:** the channel depends on HFS actually committing writes. `FlushVol` forces this, but if a volume is full / dismounted / read-only the write silently fails. If the log file exists but is truncated or stale after a crash, the HFS journal didn't catch up — retry the crash with a different volume or add an extra tick of delay after each write.
- Don't replace existing `MS_LOG` call sites with `macsurf_debug_log_writef` unless you need format arguments. `MS_LOG(literal)` is ergonomically equivalent and keeps the title bar updated for free.

## Docs

- [docs/macsurf-architecture.md](docs/macsurf-architecture.md) — Full platform architecture: rendering modes, proxy services, template system, milestone plan
- [docs/research/architecture-inventory.md](docs/research/architecture-inventory.md) — Snapshot of what currently exists in the repo and on the proxy host (no decisions, just facts)
- [docs/research/window-architecture-2026-04-22.md](docs/research/window-architecture-2026-04-22.md) — Window-framework architecture research (fixes161). Full state/event/redraw/scroll inventory of the Mac OS 9 frontend; architectural problem list; proposed unified window-state model; 6-round refactor plan (fixes162–fixes166). **fixes162+ follow this plan.**
- [docs/status.md](docs/status.md) — Project status, milestones, test environment
- [docs/codewarrior-setup.md](docs/codewarrior-setup.md) — How to install CodeWarrior 8 and build on a real Power Mac
- [docs/deploying-proxy.md](docs/deploying-proxy.md) — How to deploy the Go proxy

## SheepShaver as a Testing Tool

MacSurf is built on Linux but target-tested on real OS 9 hardware. SheepShaver (an OS 9 emulator) is a useful *partial* substitute — **not a full one**.

- **SheepShaver setup lives at** `/home/patrick/Webs/MAC/sheepshaver/` — shared folder at `shared/`, prefs at `prefs`, Xvfb on `:99`. Shared folder uses `.finf/` (32-byte FInfo per file) + `.rsrc/` (raw resource fork) sidecars for Mac metadata.
- **Run the SheepShaver AppImage** from `/tmp/squashfs-root/AppRun` with `DISPLAY=:99 APPIMAGE=/tmp/squashfs-root HOME=/home/patrick`.
- **Decode a .hqx build into the shared folder** with `/tmp/binhex_decode.py <hqx> <shared-folder>`. Writes the file + `.finf/<name>` + `.rsrc/<name>` with correct APPL/MPLS type/creator and the full cfrg/carb/SIZE resource fork.
- **Hand-built resource forks don't work.** Shipping just the PEF data fork (the `/home/patrick/Webs/macsurf/MacSurf` checkin) leaves out `cfrg` (Code Fragment resource) and OS 9 refuses to launch it. The CW8 build-on-Mac workflow produces the full resource fork at link time; replicating it on Linux requires either the .hqx or reconstructing cfrg by hand (the hand-built variant was attempted and OS 9 still rejected the launch).

**What SheepShaver IS useful for:**
- Smoke test — does the build launch at all, does Carbon init succeed, do OT/CarbonLib dependencies resolve
- Rendering regression checks — does MacTrove render, does var() still resolve, does layout not regress
- Non-hardware-specific logic bugs — compile errors that make it through Linux syntax check but not the real linker, obvious toolbox misuse

**What SheepShaver is NOT useful for:**
- Hardware-specific crashes (wheel crash, scroll-bar click crash). SheepShaver's CarbonLib + Control Manager emulation is more forgiving than real hardware. A green light in SheepShaver does NOT mean the G3/G4 will also be green.
- USB Overdrive interactions — doesn't exist in the emulator
- Real network behavior — `/home/patrick/.sheepshaver_prefs` as shipped has no usable ethernet config, so the initial fetch to the proxy at `116.202.231.103:8765` blocks until timeout (~2 min) without yielding. This is a test-env artifact, not a MacSurf bug.
- Timing-sensitive behavior — JIT / coop-scheduler pacing differs from real PPC

**Workflow:** use SheepShaver opportunistically — launch a new build, confirm it boots, click around. If anything hardware-specific is the suspect, move to real G3/G4. Don't treat SheepShaver "passed" as a substitute for hardware-side verification on wheel/scroll/USB-driven bugs.

## Known Gotchas

- **`kInitOTForApplicationMask` and `kOTInvalidConfigurationRef` are not defined in CW8's OT headers.** Either `#define` them manually (`kInitOTForApplicationMask = 0x00000002`) or avoid them entirely by using the plain `InitOpenTransport()` path.
- **Including `<OpenTransport.h>` is safe** now that `kWindowStandardHandlerAttribute` has been removed from `CreateNewWindow`. An earlier crash that seemed like it was caused by including the header was actually the window-attribute bug manifesting later.
- **No `'carb'` resource → OTClientLib crash at a fixed address.** If the same instruction crashes every time somewhere inside OTClientLib, the cause is almost always that the binary is not a recognized Carbon fragment. Add `'carb'` before debugging anything else.
- **CW8 C89:** no `inline`, no `//` comments, no variadic macros, no forward enum declarations, no C99 designated initializers, no `for (int i...)`. All variables at the top of their enclosing block.
- **Carbon partition must be at least 16 MB preferred.** Set in CW8 under "PPC PEF" → Application Heap Size / Minimum Heap Size (`MWProject_PPC_size` / `MWProject_PPC_minsize` in the `.mcp` XML). A 4 MB partition (the CW8 default) runs out mid-CSS-cascade on a moderately-sized real page — `css_select_style` returns `CSS_NOMEM` somewhere around element 380. libcss allocates via raw `malloc`/`calloc` with no NetSurf wrapper, so OOM in libcss really does mean OS-heap exhaustion. Classilla's default is 32 MB; 16 MB is MacSurf's floor. See [docs/research/state-survey-2026-04-18.md](docs/research/state-survey-2026-04-18.md) §2.
- **CW8 PPC miscompiles `long long` / `int64_t` multiply-by-constant.** `(long long)a * small_const` writes `a >> log2(const)` into the high word instead of the correct `(a*const) >> 32`. Confirmed on real hardware via probe G (fixes113): `(long long)131072 * 1024LL` produced hi=128, lo=134217728 — full product 549,890,031,616 instead of 134,217,728. This broke every FDIV/FMUL in libcss for weeks and masqueraded as a layout bug. **Mitigation:** route 64-bit fixed-point math through `double` under `#ifdef __MWERKS__`. PPC has a hardware FPU and IEEE 754's 52-bit mantissa covers every int32 fixed-point intermediate. See [browser/netsurf/include/libcss/fpmath.h](browser/netsurf/include/libcss/fpmath.h) (fixes114) for the reference pattern. Pure int32 multiplies and divides are fine — the miscompile is specifically the 64-bit shift-multiply path. **Any code doing `int64_t` or `long long` fixed-point math on CW8 PPC is suspect** and needs the same treatment or a confirmation that operands stay small enough that the miscompilation is harmless (e.g. `INTTOFIX(128)` happened to work because `128 >> 10 = 0`, which is the correct hi word by coincidence).
- **Mac CR line endings** are required for all `.c` / `.h` / `.r` files in the project. Convert with `sed 's/$/\r/' | tr -d '\n'` before packaging.
- **TextEdit (`TENew` / `TEDispose`) crashes with dsMemWZErr if WRefCon is not initialized before the first call.** The crash happens because `GetWRefCon` returns garbage on a fresh window and TextEdit dereferences it. Safe pattern: `SetPort(window)` then `SetWRefCon(window, 0)` (or to a valid struct pointer) before calling `TENew`. Once this is set, TextEdit is fully usable for the URL field and other text input widgets.
- **`kWindowStandardHandlerAttribute`** intercepts update events and leaves windows blank. Do not pass it to `CreateNewWindow`.
- **Synchronous `browser_window_schedule_reformat` during resize causes infinite layout loops.** Never call reformat directly from the grow box handler. Instead set a `needs_reformat` flag on `struct macsurf_window` and handle it in the next `nullEvent` pass. Add a `reformat_in_progress` re-entrancy guard that logs and returns if a reformat call arrives while one is already running.
- **TextEdit field activation requires explicit `TEActivate` on window activation and `TEIdle` on every `nullEvent` pass** for the caret to blink and the field to accept keystrokes. `TEKey` must be gated by a `url_field_active` flag so Return and Escape don't accidentally route as typed characters.
- **libcss lexer tokenizes `--foo` as two tokens without the keystone fix.** The original CSS 2.1 grammar allowed one leading dash for vendor prefixes. Custom properties use two. libcss's `CDCOrIdentOrFunctionOrNPD` state needs a branch where `--` followed by `startNMStart(c)` appends and continues into `IdentOrFunction` rather than rewinding to emit CHAR. Without this, the 19 `:root` definitions and 219 `var()` references in a typical modern theme drop at tokenization before any parser logic runs. Fixed in fixes139 ([browser/libcss/src/lex/lex.c](browser/libcss/src/lex/lex.c)).
- **Force-sticky title bar probes clobber each other, last writer wins.** If multiple rounds of code add `macsurf_debug_set_title_force` or `log_int_force` probes without stripping predecessors, the latest writer overwrites everything earlier in the same reformat cycle. Non-force `MS_LOG` cycling through different labels (e.g. `plot rect ↔ plot clip`) indicates no sticky is latched, which usually means the expected code path is dead. Strip upstream stickies before adding new diagnostics.
- **Fix zips only refresh the files they ship.** If a diagnostic probe was added to file X in an earlier round and subsequent zips don't ship X, the probe persists on the Mac across rounds even after removal from the Linux tree. Phantom output with no Linux-grep hit means the Mac copy of the file is out of sync with Linux. Ship the affected file explicitly to resync (fixes137 did this for `html.c` / `layout.c` / `box_construct.c`).
- **`row-gap` shares computed-style storage with `column-gap` (fixes148).** `css__parse_gap` and `css__parse_row_gap` both emit `CSS_PROP_COLUMN_GAP` bytecode. Consequences: (a) single-value `gap: N` and standalone `row-gap: N` both work as expected because they set one value the layout reads on both axes; (b) two-value `gap: A B` loses the first value — column-gap ends up equal to `B` and `A` is discarded; (c) `css_computed_row_gap` accessor does NOT exist — `layout_flex.c` reads `css_computed_column_gap` for both axes (`ctx->main_gap` and `ctx->cross_gap` both derive from it). Full-fidelity split requires adding CSS_PROP_ROW_GAP as a real property: ~17 files incl. the bit-packed `autogenerated_computed.h` struct, where a new field must be allocated in a free bit slot (word 14 has ~16 free bits). That work was scoped and deferred — it is not a 1-round change because offset miscounts in propset.h/propget.h silently corrupt unrelated properties. Defer until a dedicated bit-packing audit round.
- **UPP macro override on CarbonLib is unsafe — don't cast function pointers to UPPs.** CarbonLib's `TrackControl` / `InstallEventHandler` / etc. dispatch action procs through MixedMode: the UPP argument is expected to be a **RoutineDescriptor** (pre-Carbon style, still used by CarbonLib 1.x), not a raw PPC function pointer. Overriding `NewControlActionUPP(proc)` to `((ControlActionUPP)(proc))` — on the theory that "Carbon UPPs are just native function pointers" — is correct only for Mach-O Carbon.framework on Mac OS X, **not** for CarbonLib on OS 9. Under CarbonLib, MixedMode reads "descriptor fields" from the function-entry bytes, resolves a routine address (typically 0 or garbage), and executes `bl 0`. Crash signature: PC in very low memory (e.g. `0x00000008`) with LR matching (e.g. `0x00000004`), because `bl` at address 0 sets LR=4 and CPU walks forward through low-memory globals until first illegal opcode. CurApName is often **`CodeWarrio...`** because CW runtime captures the low-memory trap. The override in `browser/netsurf/frontends/macos9/window.c` was removed in fixes147 after it caused scroll-bar clicks to crash on every attempt. **Correct approach:** avoid the action-UPP path entirely — call `TrackControl(ctrl, pt, NULL)` and respond on return using the `ControlPartCode` from `FindControl` plus `GetControlValue()` for live-tracking CDEFs. Or if a UPP is genuinely required, let CW8's Universal Interfaces expand the macro normally (CarbonLib does export `NewRoutineDescriptor` / `DisposeRoutineDescriptor`; they are deprecated in Mach-O Carbon but present and functional in CarbonLib). See [browser/netsurf/frontends/macos9/window.c](browser/netsurf/frontends/macos9/window.c) `macos9_window_handle_scrollbar_click`.
- **Appearance live-tracking scroll bar CDEF (`kControlScrollBarLiveProc = 386`) is unsafe on real G3/G4 hardware.** `TrackControl(ctrl, pt, NULL)` on a proc-386 control crashed into MacsBug with IDENT pointing at an Appearance Manager internal symbol (`hD*` prefix). The crash is not reproducible in SheepShaver — emulated CarbonLib tolerates the same call path that real hardware rejects. fixes147's UPP macro override removal did not close the crash; fixes159 swapped to the non-live Appearance CDEF (`kControlScrollBarProc = 384`), which is crash-free because its CDEF does not do per-frame app callbacks during thumb drag. The live-track path that proc 386 uses on real hardware appears to corrupt or dispatch through bad state that only manifests outside the emulator. **If a new Control Manager feature needs live-track behavior, do NOT reach for proc 386 without a reproducer and a MacsBug trace** — the crash is hardware-specific, not reproducible from Linux, and the only path forward is direct-hardware `wh`/`sc`/`ip` capture to identify the corrupted state inside the CDEF. See [browser/netsurf/frontends/macos9/window.c](browser/netsurf/frontends/macos9/window.c) near `macos9_window_handle_scrollbar_click` and the two `NewControl` calls for the current setup.
- **Carbon event classes have per-environment availability — check Apple's `CarbonEvents.h` before registering any handler.** Events added in Mac OS X 10.0+ that were never back-ported to CarbonLib (e.g. `kEventMouseWheelMoved`) will register without error but never dispatch, and CarbonLib's dispatcher destabilizes when something downstream tries to deliver an event whose class it doesn't know. The handler code will look correct in review (pascal calling convention, proper UPP, initialized EventTypeSpec, explicit return paths — all five "classic bugs" can be absent), run in hardware tests as "no crash from our code," and get blamed for illegal-instruction crashes at heap-looking addresses that are actually CarbonLib walking uninitialized dispatch state. Apple's `CarbonEvents.h` marks each event enum with either `Mac OS X: 10.x+ in Carbon.framework` AND `CarbonLib: 1.x+`, or `CarbonLib: not available` — respect the annotation. If `CarbonLib: not available`, the platform cannot deliver that event at all, and the correct fix is to not install a handler, not to debug the handler. See [browser/netsurf/frontends/macos9/macos9_wheel.c](browser/netsurf/frontends/macos9/macos9_wheel.c) and [docs/usb-overdrive.md](docs/usb-overdrive.md) for the wheel-event case study (fixes134 → fixes140).

## CLAUDE.md Maintenance

**This file must be kept current. It falls out of date fast when not actively maintained, and stale context causes agents to repeat solved problems.**

Update CLAUDE.md as part of every round that changes project state:

- When a blocker is resolved, remove it from "Known Issues" or "Current Blocker" immediately
- When a new class of bug is identified (like CW8 PPC `long long` miscompile), add it to "Known Gotchas" with the concrete reference pattern
- When a new subsystem lands (JS engine, chrome, image handlers), add a top-level section for it
- When a file count or project structure changes materially, update the "Project File List" section
- When the build state advances (v0.2 → v0.3 etc.), update the "Build State" section

The goal is that any new agent reading CLAUDE.md at the start of a session has an accurate picture of where the project actually stands — not where it was three rounds ago. If the file has drifted from reality, fix it before doing any new work.