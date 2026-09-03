# CodeWarrior Project Settings

This page is the reference for how MacSurf's CodeWarrior project is actually configured: the exact target settings, the access-path tree, the prefix file, the application partition, the libraries it links, and the resource file that makes it a real Carbon app. It's for anyone opening `MacSurf.mcp` in CodeWarrior Pro 8 and wanting to confirm their settings match the source of truth — or for someone setting up a comparable project from scratch and wanting a worked example.

If you haven't installed the toolchain yet, start with [Setting Up the Build Environment](Setting-Up-the-Build-Environment) and the build walkthrough in [Building MacSurf](Building-MacSurf). This page assumes CodeWarrior Pro 8 (8.0 plus the 8.1/8.2/8.3 updaters) is already running on your Mac. The values below come straight from the current project file, so when in doubt, the project wins.

---

## How CodeWarrior thinks about a project

A CodeWarrior project (`.mcp` file) holds one or more **build targets**. A target is a list of source files plus the settings that turn them into one output file — which compiler, what warnings, where the headers live, what the result is called. MacSurf has a single target named `MacSurf`. Everything below lives inside that target's settings, reached from the IDE through **Edit > MacSurf Settings…** (Cmd-J).

Two things make this project unusual, and both are worth understanding before you touch a setting:

First, CodeWarrior compiles MacSurf in **C89** mode and the code leans on a single injected prefix file to paper over the platform. Change the prefix file path and nothing builds.

Second, the project pulls together roughly **850 `.c` files** spread across a deep folder tree, and CodeWarrior finds them through an ordered list of **access paths** (its name for include/search directories). The order matters — a header found early can shadow a header found later — and that shadowing is load-bearing, not accidental. We use it on purpose so our Mac-specific shim headers win over the NetSurf originals. More on that below.

---

## Target settings (the exact values)

These are the scalar settings from the project's `MacSurf` target. Where a setting maps to a panel in the settings dialog, the panel is named.

| Setting | Value | Panel |
|---|---|---|
| Linker | MacOS PPC Linker | Target Settings |
| Target name | `MacSurf` | Target Settings |
| Project type | Application | PPC Target |
| Output / fragment name | `MacSurf` | PPC PEF |
| File creator | `MPLS` | PPC Target |
| File type | `APPL` | PPC Target |
| C++ source | off (C only) | C/C++ Language |
| Struct alignment | `MC68K` (68K alignment) | PPC Processor |
| Processor | Generic | PPC Processor |
| Instruction scheduling | off | PPC Processor |
| Prefix file | `macsurf_prefix.h` | C/C++ Preprocessor |
| Generate SYM file | on (full paths) | PPC Linker |
| Generate link map | off | PPC Linker |

A few of these deserve a *why*, because they're the ones people get wrong.

**File creator `MPLS`, file type `APPL` — and the case matters.** On Classic Mac OS, a file's type and creator are four-character codes the Finder uses to know what a file *is* and which app owns it. `APPL` marks the binary as an application; `MPLS` is MacSurf's creator signature (the org is "mplsllc"). These codes are **case-sensitive**: `MPLS` and `mpls` are different creators, and getting the case wrong means the Finder won't pair the app with its documents and icon correctly. Type it exactly as shown.

**Struct alignment is `MC68K`, not PowerPC.** This surprises people, because the target is PowerPC. PowerPC's natural alignment would pad many structs differently. We use 68K alignment because it matches what the classic Macintosh Toolbox headers expect — the OS data structures were laid out in the 68K era and the Universal Interfaces assume 68K packing. Mixing alignment conventions across a struct boundary is how you get fields that read garbage. Leave this on `MC68K`.

**Processor is Generic.** CodeWarrior can tune codegen for a specific PowerPC chip (the 750/G3, the 7400/G4, and so on). MacSurf targets the whole G3/G4 range, so we tell it to emit generic PowerPC code that runs everywhere rather than scheduling instructions for one pipeline. Instruction scheduling is off for the same reason.

**C++ is off, and the dialect is "C89 with a few extensions tolerated."** NetSurf is C and the frontend is C. The project has CodeWarrior's **C99 Extensions enabled** (`MWFrontEnd_C_c99 = 1`), with ANSI-strict and ANSI-keywords-only both off (`MWFrontEnd_C_ansistrict = 0`, `MWFrontEnd_C_onlystdkeywords = 0`). So the compiler is *not* configured to reject extensions — which is exactly what lets the code lean on conveniences like `__VA_ARGS__` variadic macros. Don't read that as license to write modern C, though: CW8's C99 support is partial, and parts of it are actively broken on this compiler. The codebase — and every vendored NetSurf library, all ported down to compile here — holds to a C89 discipline that matches NetSurf's own house style: no `//` comments, no C99 designated initializers, no `for (int i …)` loop-scope variables, no `inline`, declarations at the top of each block. Before you edit code, read [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas); the landmines are real and catalogued there.

### The prefix file

CodeWarrior can `#include` one header before *every* translation unit, configured under **C/C++ Preprocessor** as the prefix file. MacSurf points this at `macsurf_prefix.h`, which lives in the frontend folder. It's a file, not a block of inline prefix text — the project references it by name and CodeWarrior finds it on the access paths.

That one file is where the platform gets bootstrapped. It defines `__MACOS9__`, `NO_IPV6`, and `TARGET_API_MAC_CARBON`, and it pulls in `<MacTypes.h>` as its very first line so the Mac toolbox's `Boolean`/`true`/`false` definitions land before anything else can clash with them. Because it's injected everywhere, a change here recompiles the world — and a wrong path here makes nothing build at all. If you're curious about everything it does (including the debug-log gating and a typedef it has to supply by hand), the full story is in `CLAUDE.md` under "Prefix File" and the gotchas page.

### Application partition + stack: MacSurf wants a big one

Classic Mac OS doesn't have virtual memory the way you're used to. Each application gets a fixed **memory partition** carved out of system RAM when it launches, set in the project's PPC PEF panel (Application Heap Size / Minimum Heap Size), plus a reserved **stack** within it. As of **v1.5** the project asks for a large one deliberately:

| Setting | PPC PEF field | Value (K) | ≈ |
|---|---|---:|---:|
| `MWProject_PPC_size` | Preferred (Application Heap) | **409600** | ~400 MB |
| `MWProject_PPC_minsize` | Minimum Heap | **307200** | ~300 MB |
| `MWProject_PPC_stacksize` | Stack | **16384** | ~16 MB |

**Why it grew (and why the stack matters now).** Two things in the modern-JS path are memory- and stack-hungry on heavy pages:

- **The CSS engine + DOM.** libcss allocates with raw `malloc`/`calloc` and has no graceful fallback when the OS heap runs dry; on a moderately complex page a too-small partition runs out mid-cascade and `css_select_style` returns `CSS_NOMEM`, which looks like a layout bug but is really starvation.
- **JS-built rendering (the re-convert renderer).** When JavaScript mutates the DOM, MacSurf rebuilds the box tree — briefly holding *two* box trees at once — and box construction **recurses over the DOM**, which on a JS-built page is deeper than the original markup. On a ~10 MB modern page that combination overran the old ~195 MB heap / ~9 MB stack and hard-crashed the machine (a failed allocation / smashed return chain showing up as a garbage jump). The fix wasn't code — it was giving the app room: ~400 MB heap and a 16 MB stack on a RAM-rich Mac made it stable.

**What to expect by RAM.** Set the partition as high as your Mac's RAM comfortably allows; if the machine has *less* than the preferred size, the app won't launch until you lower it.

| Your Mac's RAM | Suggested heap / stack | What works |
|---|---|---|
| **512 MB+** (the dev iMac is 640 MB) | ~400 MB / 16 MB | Everything, including JS-built content on heavy modern sites |
| **256 MB** | ~180 MB / 12 MB | Most sites; the heaviest JS-built pages may stall or run out |
| **128 MB** | ~96 MB / 8 MB | Lighter modern + retro sites; reconvert on heavy SPAs not viable |
| **64 MB (floor)** | ~16 MB / 8 MB | Basic CSS pages render; below this libcss starves and pages come out half-formed or blank |

The **16 MB heap / 8 MB stack floor** still holds for basic rendering (Classilla, for comparison, defaults to 32 MB), but JS-heavy modern sites genuinely need the headroom. On a small machine, expect retro/text sites to work well and the heaviest script-built sites to be out of reach — that's a RAM limit, not a bug.

---

## Linked libraries and the resource file

Five non-source items round out the build. They sit alongside the `.c` files in the project window:

- **`MSL_C_Carbon.Lib`** — the Metrowerks Standard Library (MSL), Carbon flavor. This is CodeWarrior's C runtime: `malloc`, `memcpy`, the string functions, the parts of the C standard library MacSurf actually uses. The "Carbon" build is the one that links against CarbonLib rather than the old InterfaceLib.
- **`MSL_Runtime_PPC_D.Lib`** — the MSL PowerPC runtime support (program startup/teardown, the low-level glue between the C runtime and the PEF/CFM loader). The `_D` suffix is Metrowerks' debug-runtime variant of the library.
- **`CarbonLib`** — the stub library for Carbon. You link against the stub at build time; at runtime the real `CarbonLib` shared library in the System Folder's Extensions provides the implementation. This is what lets one binary run on OS 9 (via CarbonLib) and on early Mac OS X.
- **`MacSurf.rsrc`** — the resource file. CodeWarrior links `.rsrc` files straight into the output's resource fork, no Rez step. This one carries the single most important resource MacSurf has: the **`'carb'`** marker, plus the application's icon family.

That `'carb'` resource earns its own paragraph. Classic Mac OS uses the Code Fragment Manager to load PEF binaries, and without a `'carb'` resource the loader treats the binary as a plain classic (pre-Carbon) fragment. CarbonLib then never loads as a dependency, and the first Carbon call that needs an initialized CarbonLib context — typically an Open Transport `*InContext` call — runs against uninitialized state and crashes at a fixed address inside the OT client library. The fix is not in code; it's the resource. The `'carb'` resource (with the icon family, `FREF`, and `BNDL`) is what tells the system "this is a Carbon app, load CarbonLib." If MacSurf crashes identically every launch somewhere inside OTClientLib, suspect a missing `'carb'` before anything else. The resource-fork pipeline that builds `MacSurf.rsrc` is documented in the repo's `docs/resources.md`.

> **Note on output format.** CodeWarrior for Classic Mac OS produces **PEF** (Preferred Executable Format) executables — also called CFM files, because the Code Fragment Manager loads them. That's the normal, expected output for a PowerPC OS 9 app; there's no separate "make it PEF" switch to find. ([PEF on Wikipedia](https://en.wikipedia.org/wiki/Preferred_Executable_Format))

---

## The access-path tree

Access paths are CodeWarrior's search directories — its equivalent of `-I` include paths plus library search paths, kept as an *ordered* list. There are two lists per target: **User** paths (your project's folders) and **System** paths (the compiler's own headers and libraries). CodeWarrior searches them top to bottom and the first match wins, which is exactly why ordering matters here.

The MacSurf project carries **55 user paths and 19 system paths**. They form a hierarchical tree mirroring the source layout — `…:libcss:src:select:properties:`, `…:netsurf:content:handlers:html:`, and so on. Almost all are **non-recursive** (a path matches files directly in that folder, not its subfolders), which is why every library subdirectory gets its own entry. Older MacSurf docs describe a single "flat folder" of files; that's out of date — the real project is this deep tree, and you should follow it.

### Two root tokens: `{Project}` and `{Compiler}`

CodeWarrior lets a path be **relative** to one of a few named roots, so the project isn't pinned to one machine's disk layout. Two roots matter for MacSurf:

- **`{Project}`** — the folder containing the `.mcp` file. A path under `{Project}` follows the project wherever you move it.
- **`{Compiler}`** — the CodeWarrior installation folder. Paths under `{Compiler}` point at the MSL libraries and the Universal Interfaces, so they keep working no matter which disk CodeWarrior lives on.

A path can also be **absolute** (anchored to a specific volume by name). The current project happens to use absolute paths for most of the source tree, rooted at the maintainer's working volume. **You'll want to re-point these to your own disk.** Below, the maintainer's private volume and folder names are generalized to `<YourDisk>` and `<your-source-folder>` — substitute the real name of *your* disk (the volume name as it appears on your desktop) and wherever you unpacked the source.

### User paths — the source tree

The user paths break down by component. Rather than list all 55 verbatim, here's the shape, with representative entries written in the generalized form. A Mac HFS path uses colons as separators, and the leading element is the volume name.

**The NetSurf engine and Mac frontend** (`netsurf/…`):

```
<YourDisk>:<your-source-folder>:netsurf:
<YourDisk>:<your-source-folder>:netsurf:include:
<YourDisk>:<your-source-folder>:netsurf:content:
<YourDisk>:<your-source-folder>:netsurf:content:handlers:
<YourDisk>:<your-source-folder>:netsurf:content:handlers:css:
<YourDisk>:<your-source-folder>:netsurf:content:handlers:html:
<YourDisk>:<your-source-folder>:netsurf:content:handlers:javascript:
<YourDisk>:<your-source-folder>:netsurf:desktop:
<YourDisk>:<your-source-folder>:netsurf:frontends:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:shims:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:sys:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:arpa:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:netinet:
<YourDisk>:<your-source-folder>:netsurf:frontends:macos9:javascript:
<YourDisk>:<your-source-folder>:netsurf:utils:http:
<YourDisk>:<your-source-folder>:netsurf:utils:nsurl:
```

The `frontends:macos9:` folder and its `shims:`, `sys:`, `arpa:`, `netinet:` subfolders hold the Mac-specific code and the POSIX/header shims. Their position in the list (ahead of the deeper NetSurf paths) is what lets a shim header shadow the NetSurf original of the same name — see "Why path order matters" below.

**The five ported NetSurf libraries**, each with its own `include:` and `src:` subtrees:

```
<YourDisk>:<your-source-folder>:libcss:include:
<YourDisk>:<your-source-folder>:libcss:src:
<YourDisk>:<your-source-folder>:libcss:src:charset:
<YourDisk>:<your-source-folder>:libcss:src:lex:
<YourDisk>:<your-source-folder>:libcss:src:parse:
<YourDisk>:<your-source-folder>:libcss:src:parse:properties:
<YourDisk>:<your-source-folder>:libcss:src:select:
<YourDisk>:<your-source-folder>:libcss:src:select:properties:
<YourDisk>:<your-source-folder>:libcss:src:utils:
<YourDisk>:<your-source-folder>:libdom:include:
<YourDisk>:<your-source-folder>:libdom:bindings:hubbub:
<YourDisk>:<your-source-folder>:libdom:src:core:
<YourDisk>:<your-source-folder>:libdom:src:events:
<YourDisk>:<your-source-folder>:libdom:src:html:
<YourDisk>:<your-source-folder>:libdom:src:utils:
<YourDisk>:<your-source-folder>:libhubbub:include:
<YourDisk>:<your-source-folder>:libhubbub:src:
<YourDisk>:<your-source-folder>:libhubbub:src:charset:
<YourDisk>:<your-source-folder>:libhubbub:src:tokeniser:
<YourDisk>:<your-source-folder>:libhubbub:src:treebuilder:
<YourDisk>:<your-source-folder>:libhubbub:src:utils:
<YourDisk>:<your-source-folder>:libparserutils:include:
<YourDisk>:<your-source-folder>:libparserutils:src:
<YourDisk>:<your-source-folder>:libparserutils:src:charset:
<YourDisk>:<your-source-folder>:libparserutils:src:charset:codecs:
<YourDisk>:<your-source-folder>:libparserutils:src:charset:encodings:
<YourDisk>:<your-source-folder>:libparserutils:src:input:
<YourDisk>:<your-source-folder>:libparserutils:src:utils:
<YourDisk>:<your-source-folder>:libwapcaplet:include:
<YourDisk>:<your-source-folder>:libwapcaplet:src:
```

**The JavaScript engine** (`libduktape/`):

```
<YourDisk>:<your-source-folder>:libduktape:
```

**The native TLS stack (macTLS / BearSSL).** macTLS lives in its own folder tree (separate from the NetSurf source) and is linked in via three paths — the OS 9 glue, the BearSSL sources, and the BearSSL headers. The headers path is one of the few **recursive** entries:

```
<YourDisk>:<your-mactls-folder>:os9:
<YourDisk>:<your-mactls-folder>:bearssl:src:
<YourDisk>:<your-mactls-folder>:bearssl:inc:        (recursive)
```

This is how MacSurf does HTTPS itself — native TLS 1.3 over Open Transport, direct to the origin server. The details are on [Networking & TLS](Networking-and-TLS).

Two `{Compiler}`-relative library paths also live in the user list (CodeWarrior allows that), pointing at the MSL PPC libraries and the Universal stub libraries:

```
{Compiler}:MSL:MSL_C:MSL_MacOS:Lib:PPC:
{Compiler}:MacOS Support:Universal:Libraries:PPCLibraries:
```

And finally the project root itself, `{Project}`, is on the list as a recursive path so the `.mcp` folder's own contents are always reachable.

### System paths — CodeWarrior's own headers and libraries

The 19 system paths are mostly `{Compiler}`-relative and point at the toolchain's headers, MSL includes, and runtime/stub libraries. The load-bearing ones:

```
{Compiler}:MacOS Support:MacHeaders:
{Compiler}:MSL:MSL_C:MSL_Common:Include:
{Compiler}:MSL:MSL_Extras:MSL_Common:Include:
{Compiler}:MSL:MSL_C:MSL_MacOS:Include:
{Compiler}:MacOS Support:Libraries:Runtime:Libs:
{Compiler}:MacOS Support:Universal:Libraries:StubLibraries:
```

There's also an absolute recursive path pointing at the `MacOS Support` folder inside the CodeWarrior install. If you re-point the user paths to your own disk, glance at the system paths too: any that are absolute (rather than `{Compiler}`-relative) need the same treatment so they find the headers on *your* CodeWarrior installation.

The remaining slots in the 19-entry system list aren't load-bearing. A few are empty placeholder rows that CodeWarrior leaves in the list (blank `{Project}` and absolute entries), and one is a leftover absolute path pointing at an unrelated folder — debris from an earlier project layout, not something the build needs. The entries that matter are the ones shown above: the `{Compiler}`-relative MSL, MacHeaders, and library paths, plus the recursive `MacOS Support` folder. You can leave the empty and stray rows alone.

### Why path order matters (the shadowing trick)

The MacSurf frontend folder sits ahead of the deep NetSurf header folders in the user-path list, and that's deliberate. When a translation unit does `#include "something.h"`, CodeWarrior walks the access paths in order and takes the first match. By placing `frontends/macos9/` (and its subfolders) early, a Mac-specific shim header with the same name as a NetSurf header gets found first and **shadows** the original. That's how we substitute OS 9 versions of POSIX headers (`sys/time.h`, `dirent.h`, and friends) and stub out dependencies NetSurf expects but OS 9 doesn't have.

The flip side is a sharp edge worth knowing: a stub header that sets the *real* header's include guard but doesn't define the structs the real header would have defined will silently break every file downstream. The defense is to give each forwarding shim its own guard and have it `#include` the real header by full path. That whole class of trap — and the fixes — is catalogued on [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas). If you add or reorder access paths, keep the frontend/shim folders ahead of the NetSurf folders.

---

## The shape of the project: ~850 files

The project window lists about **850 `.c` files** plus the three libraries and the one `.rsrc`. That count climbs as features land; it isn't a fixed number. You don't need to know every file, but it helps to know the *groups*, because that tells you where a given symbol comes from when the linker complains. Roughly:

| Group | What it is | Rough size |
|---|---|---|
| `libcss` | CSS parser, selection, and the cascade. The biggest single chunk: per-property parse (`p_*.c`), select (`s_*.c`), and autogenerated property tables (`autogenerated_*.c`, `ag_*.c`). | ~300 files |
| `libdom` | The DOM implementation — every HTML element type (`html_*_element.c`), nodes, events, collections. | ~95 files |
| `libhubbub` | The HTML5 parser: tokeniser, tree-builder, the insertion-mode files (`in_body.c`, `in_table.c`, …). | ~30 files |
| `libparserutils` | Low-level parsing + charset codecs (`codec_utf8.c`, `codec_8859.c`, …). | ~15 files |
| `libwapcaplet` | String interning. | a couple of files |
| NetSurf core | The engine glue: `content/` (`hlcache.c`, `llcache.c`, `fetch.c`), `desktop/` (`browser.c`, `textarea.c`, `scrollbar.c`), `utils/` (`nsurl.c`, `talloc.c`, `messages.c`). | ~30 files |
| NetSurf content handlers | The HTML, CSS, and JS content handlers (`html.c`, `layout.c`, `redraw.c`, `html_css.c`, `js_content.c`). | ~40 files |
| `macos9` frontend | The Mac-specific code: `main.c`, `window.c`, `plotters.c`, `macos9_font.c`, `macos9_bitmap.c`, `macos9_image.c`, the fetchers, and the POSIX shims (`mac_*.c`). | ~50 files |
| `libduktape` | The JS engine — `duktape.c` plus the MacSurf JS glue (`macsurf_js*.c`). | a handful of large files |
| macTLS / BearSSL | The native TLS stack: BearSSL crypto primitives (the many short `i31_*.c`, `ec_*.c`, `rsa_*.c`, `ssl_*.c` files) plus the OS 9 glue (`ostls_*.c`) and the TLS 1.3 handshake/key-schedule/record files. | ~200 files |

The volume of tiny files in `libcss` and BearSSL is normal: both projects deliberately split one function or one property per file. That's why the access-path tree has so many leaf folders — each subdirectory full of single-purpose `.c` files needs its own non-recursive path.

For the verbatim scalar settings and the full file list, see the staged facts in the repo (`wiki/.facts/`); this page summarizes them.

---

## Adding a new `.c` file

When you write a new source file, CodeWarrior won't pick it up on its own — there's no autodiscovery. You add it by hand, and there are two steps that are easy to forget:

1. **Make sure the file's folder is on an access path.** If you drop `my_feature.c` into a folder that has no (non-recursive) access-path entry, the compiler can't find files it references and the linker can't find the object. If it's a brand-new folder, add an access path for it.
2. **Add the file to the project window.** Project menu **Add Files…**, navigate to the containing folder (not a full file path), select the `.c` file from the dialog's file list, confirm its row is highlighted, then choose it. Until you do this, the file isn't part of the build.

The add dialog is a folder browser, not a filename field. If navigation is needed, use Go to Folder with the directory only; then select the exact source file in the list. For example, adding `macsurf_capability.c` uses the folder `netsurf/frontends/macos9/`, highlights `macsurf_capability.c`, then clicks **Choose**. Confirm the project window's file count increases by one before building. Headers do not belong in the project list; the access paths resolve them.

The trap here is subtle and has bitten this project more than once: if you add a *new* `.c` file that defines a symbol but forget to add it to the project, an *old* object file that defined the same symbol can still satisfy the linker. The build succeeds, and the new code is silently dead — the crash or wrong behavior only shows up at runtime when the feature is exercised. So whenever a change introduces a new file (a new libcss `p_*.c`/`s_*.c` pair is the classic case), call it out explicitly and add it deliberately. After file changes, do a **Remove Object Code** (Project menu) before rebuilding so CodeWarrior recompiles cleanly rather than relinking stale objects.

One project-file rule for contributors who edit on Linux: **don't hand-edit the `.mcp` on Linux and copy it over.** The project file list is maintained on the Mac side through the CodeWarrior IDE; a Linux-edited `.mcp` clobbers those local changes. Edit source on Linux all you like (see [Cross-Developing from Linux](Cross-Developing-from-Linux)), but let the IDE own the project file. When you hand a change to someone, name the new files in plain English and let them add the files and access paths through the IDE.

---

## Where to go next

- [Building MacSurf](Building-MacSurf) — the end-to-end build walkthrough, including the recommended StuffIt build pack.
- [Setting Up the Build Environment](Setting-Up-the-Build-Environment) — installing OS 9, CodeWarrior Pro 8, and CarbonLib.
- [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) — the C89/CW8 landmine list, including the access-path shadowing traps mentioned above.
- [Cross-Developing from Linux](Cross-Developing-from-Linux) — syntax-checking on Linux and moving files to the Mac.
- [Architecture Overview](Architecture-Overview) — how the libraries above fit together at runtime.

## Sources

- Project settings, access paths, and file list: the current MacSurf CodeWarrior project, staged in the repo at `wiki/.facts/` (`01-scalar-settings.txt`, `03-access-paths.txt`, `04-file-list.txt`).
- Build mechanics, the `'carb'` requirement, the partition floor, and the prefix file: repo `CLAUDE.md` and `docs/resources.md`.
- CodeWarrior project/target model, prefix-file mechanism, access-path search order, and PEF output: [NXP CodeWarrior IDE docs](https://docs.nxp.com/bundle/GUID-9FAC1C79-3809-474F-B8DF-82BEB5B88419/page/GUID-E8DEA010-B02F-4048-84C1-49B6094FFFE9.html); [PEF (Preferred Executable Format), Wikipedia](https://en.wikipedia.org/wiki/Preferred_Executable_Format); CodeWarrior Pro 8 background and the 8.0→8.3 updater chain, [Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x).
