# Building MacSurf

This is the end-to-end walkthrough for turning MacSurf source into a running browser on a Power Mac. It's for a developer who's comfortable with a build system but may have never opened CodeWarrior or touched Mac OS 9. We'll go through both supported paths, what to expect when you press Build, and what you'll see the first time the app launches.

Before you start, you need a working build environment: Mac OS 9 (real or emulated), CodeWarrior Pro 8, and CarbonLib. If you don't have that yet, set it up first — see [Setting Up the Build Environment](Setting-Up-the-Build-Environment). This page assumes the toolchain is already in place and gets straight to the build.

## A word about `git clone`

A fresh checkout of the repository is **not** a turnkey build, and that's deliberate. The CodeWarrior project file (`MacSurf.mcp`) is a binary file that lives on the Mac, the source needs CR (`\r`) line endings to behave in the classic editors, and the project references a specific arrangement of access paths that only matters once the files are sitting on an HFS volume. The maintainer edits on Linux and builds on the Mac, so the Git tree is the editing surface, not the shipping artifact.

The thing you actually build from is the **StuffIt build pack** in the `builds/` directory of the repo. It's a snapshot of the whole project — source, the `.mcp` file, the shim headers — packaged the way a Mac expects it, with the resource forks and line endings already correct. That's Path A below, and it's the path we recommend for everyone. Path B (building from the raw source tree) is here for completeness and for people who are already deep in the Linux cross-dev workflow, but it's more setup for the same result.

The wiki and the build pack are best-effort, as-is documentation for a solo nights-and-weekends project. There's no promise of one-on-one build support — but if you follow Path A on a properly set up Mac, it works.

## Path A — build from the StuffIt build pack (recommended)

### 1. Get the build pack onto the Mac

In the repo's `builds/` folder you'll find a few StuffIt archives (`.sit` files). The two you care about for building are:

- **`Complete-Project-*.sit`** — the current, complete project snapshot (the date in the filename tells you when it was cut, e.g. `Complete-Project-060126.sit`). This is the freshest build pack and the one to grab.
- **`MacSurf-BuildPack.sit`** — the build-it-yourself bundle: all the MacSurf C source (the NetSurf core plus the `macos9` frontend, `libcss`, `libdom`, `libhubbub`, `libparserutils`, `libwapcaplet`, and Duktape), the `MacSurf.mcp` project file with its target settings and search paths already configured, and the shim headers that make the code compile under CodeWarrior 8's strict C89.

(There's also `MacSurf.sit` in that folder — that's the *already-compiled* application, for people who just want to run MacSurf without building it. Skip it if you're here to build.)

Grab the most recent `Complete-Project-*.sit`. Getting it onto the Mac is its own small adventure depending on your setup; the file-transfer mechanics (FAT32 thumb drive, networking, emulator shared folders) are covered in [Setting Up the Build Environment](Setting-Up-the-Build-Environment).

### 2. Expand it on the Mac

On the Mac, expand the archive with **StuffIt Expander**. (It shipped with Mac OS 9 and is on practically every OS 9 system; if yours doesn't have it — or has a copy too old to open the archive — [StuffIt Expander 5.5](https://macintoshgarden.org/apps/stuffit-expander-55) is the easiest version to get onto a bare machine.) Expanding produces a project folder with the full source tree inside.

We expand *on the Mac* rather than on Linux for a reason: StuffIt preserves Mac resource forks and the correct line endings. Unpacking the same data through a Linux unzip would silently flatten that metadata, and you'd spend an afternoon chasing phantom "file not found" and weird-character errors that aren't really there.

### 3. Open `MacSurf.mcp` in CodeWarrior 8

Find `MacSurf.mcp` in the expanded folder (it's in the `macos9` frontend directory) and double-click it. CodeWarrior 8 launches and shows the project window — a list of every source file in the project, grouped into folders in the IDE's left pane.

If CodeWarrior complains that it can't find a source file, the directory structure didn't survive transfer. The fix is to re-expand the `.sit` on the Mac with StuffIt Expander rather than copying loose files around — the project relies on its folders being where the access paths expect them.

### 4. Check the application partition

This is the setting people skip, and on a memory-tight Mac it's the one that bites. The current project ships a **large partition by default** (around 195 MB preferred), which is wonderful if your Mac has the RAM and a problem if it doesn't — an app can't launch into a partition bigger than free memory. The reason MacSurf is hungry is concrete: `libcss`, the CSS engine, allocates from the application's heap as it builds the cascade for a page. On a heavily-styled real-world site, a too-small partition runs out *mid-cascade* — `css_select_style` starts returning out-of-memory partway through, and you get a half-rendered or blank result that looks like a rendering bug but is really heap starvation. The practical floor is **16 MB preferred / 8 MB minimum**; below that, even modest pages starve.

You set this in the project's target settings. In CodeWarrior, open **Edit ▸ MacSurf Settings…** (or press Cmd-J) and find the **PPC PEF** panel. There you'll see the application heap size fields — the preferred (application) heap and the minimum heap. Set them to whatever your Mac can comfortably spare: anywhere from the project's large default down toward the 16 MB / 8 MB floor, but not below it.

The exact panel names and the full list of target settings are documented on the [CodeWarrior Project Settings](CodeWarrior-Project-Settings) page; the build pack ships with these already set correctly, so this step is a confirmation, not a configuration.

### 5. Remove Object Code

Before you build, choose **Project ▸ Remove Object Code** and remove the object code for the whole project.

Here's why this matters. CodeWarrior caches compiled object files (`.o`) between builds, which is normally a great speed-up. But the build pack may have been compiled before being packed, or files may have arrived with timestamps that make the IDE think old objects are still current. Removing object code forces a clean compile of every file, which is exactly what you want for a first build. It's a few minutes of extra compile time in exchange for not chasing ghosts. After the project is building cleanly and you're making small edits, you won't need to do this every time — but do it for the first build, and any time you change a header that lots of files depend on.

### 6. Build

Choose **Project ▸ Make** (Cmd-M) and let it run.

This is a large project — around 850 C source files, including the entire NetSurf engine, all five ported NetSurf libraries, the Duktape JavaScript engine, and the BearSSL-based macTLS stack — so a full clean build takes a while on period hardware. Compiling hundreds of files on a 233 MHz G3 is not instant. Put on a podcast.

As it compiles, CodeWarrior shows an **Errors & Warnings** window. Warnings (yellow) are fine — there are some in a codebase this size, and they don't stop the build. Errors (red) stop the file that hit them; double-click any error to jump straight to the offending line. On a clean build from the pack you shouldn't see compile errors, because the source has already been through the C89 syntax gauntlet. If you *do* hit one, the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page is the field guide to what the usual culprits are.

When the build finishes successfully, CodeWarrior writes out the application. The output is a PEF/CFM executable — Apple's classic-Mac executable format, loaded by the Code Fragment Manager — with the resource fork attached. That resource fork carries `MacSurf.rsrc`, which holds the mandatory `'carb'` resource (more on that just below) and the app's icon family. The result is a double-clickable `MacSurf` application sitting next to the project.

## Path B — build from the source tree

If you're working in the Linux cross-development workflow and want to build the live source tree rather than a packaged snapshot, you can. The catch is that you have to reconstruct on the Mac side what the build pack gives you for free: the correct line endings, the `MacSurf.mcp` project with its access paths, and the resource file.

The shape of it:

1. Get the source tree (`browser/netsurf/` and the library trees) onto the Mac with the directory structure intact, converting source files to CR line endings on the way over. The transfer and line-ending mechanics are in [Cross-Developing from Linux](Cross-Developing-from-Linux).
2. Open `MacSurf.mcp` and verify the target settings by hand — compiler in C89 mode, the `macsurf_prefix.h` prefix file, the access-path tree, the linked MSL and CarbonLib stub libraries, and the application partition (large by default; lower it toward the 16 MB floor if your Mac is short on RAM).
3. Confirm `MacSurf.rsrc` is in the project (it carries the `'carb'` marker and the icons — without it the linked binary won't be recognized as Carbon).
4. Remove Object Code, then Make.

The exact settings, the full access-path list, the prefix file contents, and the library list are all on the [CodeWarrior Project Settings](CodeWarrior-Project-Settings) page — that page is built from the current project and is the source of truth for Path B. Don't try to reverse-engineer the settings from memory; copy them from that page.

> **TODO (verify):** The repository does not ship a script that fully assembles a Path B project (line-ending conversion + project file + resource fork) in one step. Path B is effectively "do what the build pack does, by hand." If a future helper script lands, document it here.

### Why the `'carb'` resource is non-negotiable

Whichever path you take, the binary must carry the `'carb'` resource, and it's worth understanding why, because a missing `'carb'` produces a baffling crash rather than a clean failure.

Carbon is Apple's API that lets one binary run on both Mac OS 9 (through the CarbonLib system extension) and early Mac OS X. On OS 9, the Code Fragment Manager looks at a binary's resource fork to decide what it is. A plain PowerPC application and a Carbon application look nearly identical on disk — both are PEF executables with a `'cfrg'` code-fragment resource. The `'carb'` resource (ID 0) is the marker that says "this is a Carbon app." ([Apple, *Carbon Porting Guide*](https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf).)

Without that marker, CarbonLib never loads as a dependency. The app launches as a plain classic binary, and the first time it makes a Carbon call into an uninitialized CarbonLib context — which happens almost immediately, since MacSurf's networking goes through Open Transport's Carbon entry points — it crashes at a fixed address inside the system library. The symptom (a hard crash at the same address every launch, deep in a system extension) looks like a networking or memory bug; the cause is the missing marker. The build pack's `MacSurf.rsrc` includes `'carb'`, so as long as that resource file stays in the project, you're covered.

## Running the result and what to expect on first launch

Double-click the freshly built `MacSurf` application.

At startup MacSurf does the Carbon-app dance: it skips the old `InitGraf`/`InitFonts`/`InitWindows` Toolbox calls that CarbonLib handles itself, keeps `InitCursor()` and a flush of pending events, registers with the Appearance Manager (`RegisterAppearanceClient()`) so the UI draws in the OS 9 Platinum theme rather than the ancient black-and-white look, and initializes Open Transport for networking. Then the browser window comes up with the chrome you'd expect: an address bar, back / forward / reload / home buttons, a scrollbar, and a status bar along the bottom.

Type a URL and press Return. MacSurf fetches it directly — including HTTPS, which the Mac negotiates itself via the native macTLS stack (TLS 1.3 with TLS 1.2 fallback), no helper or intermediary involved. The page runs the full pipeline on-device: fetch, HTML parse, CSS cascade with native `var()` resolution, layout, and a QuickDraw paint. You'll see styled text, colors, images (PNG with real alpha, plus GIF/BMP/TIFF/JPEG), and a fair amount of modern CSS. Inline JavaScript runs too, through the embedded Duktape ES5 engine and a browser runtime wired up on the Mac itself.

A few honest first-launch notes:

- **First load can feel slow.** Every HTTPS sub-resource on a cold page does its own TLS handshake, and TLS math on a G3 is real work. A heavy first page can take a noticeable few seconds. This is the platform being twenty-five years old, not the build being broken.
- **It's not a 2026 desktop browser.** MacSurf renders a large amount of the real web and does genuinely remarkable things for the hardware — native TLS 1.3 on a 1999 machine, ~150 CSS properties consumed in layout, a real JS runtime — but heavy single-page-app frameworks and very large DOM-mutation apps are still a frontier, tracked openly in the issue tracker. A page that doesn't render perfectly isn't necessarily a build problem.
- **An emulator launch is a smoke test, not a verdict.** If you built and ran under SheepShaver, a clean launch and render tells you the build is sound, but SheepShaver is more forgiving than real hardware and its networking needs manual configuration. Hardware-specific behavior — and the truth about anything timing- or driver-sensitive — only comes from a real G3 or G4. See the build-environment page for the emulator caveats.

If the app launches, loads `home.macsurf.org` or a site of your choosing, and paints a styled page — you've built MacSurf.

## Where to go next

- [Setting Up the Build Environment](Setting-Up-the-Build-Environment) — get OS 9, CodeWarrior 8, and CarbonLib working in the first place.
- [CodeWarrior Project Settings](CodeWarrior-Project-Settings) — the exact target settings, access paths, prefix file, and libraries (the ground truth for Path B).
- [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) — what to do when the compiler complains.
- [Cross-Developing from Linux](Cross-Developing-from-Linux) — the edit-on-Linux, build-on-Mac workflow and CR line endings.
- [Diagnostics & Debugging](Diagnostics-and-Debugging) — the file-backed debug log and how to read a crash.

## Sources

- CodeWarrior Pro 8 system requirements, updater chain, and PEF/CFM output — Macintosh Garden, *CodeWarrior Pro 8.x*: https://macintoshgarden.org/apps/codewarrior-pro-8x and *Preferred Executable Format*: https://en.wikipedia.org/wiki/Preferred_Executable_Format
- The `'carb'` Carbon marker resource and CarbonLib binding — Apple, *Carbon Porting Guide* (Legacy, 2002): https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf
