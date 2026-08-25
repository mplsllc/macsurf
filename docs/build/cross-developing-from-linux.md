# Cross-Developing from Linux

MacSurf is edited on a modern Linux desktop and compiled on a Power Mac running CodeWarrior. This page is the bridge between those two worlds: how to catch most of your mistakes on Linux *before* they ever reach the Mac, what the shim layer is and why it exists, the one line-ending rule that will bite you if you forget it, and the general ways to move a file across to OS 9 without corrupting it.

If you have never built MacSurf at all yet, start with [Building MacSurf](Building-MacSurf) and [Setting Up the Build Environment](Setting-Up-the-Build-Environment). This page assumes you already have a working Mac-side build and want to do the day-to-day editing somewhere more comfortable.

## Why split the work across two machines?

The honest answer is comfort and speed. Writing C for a 1999 browser engine goes a lot faster in a 2026 editor with a real terminal, fast search, and version control that does not run on cooperative multitasking. So the editing, the grepping, the git history, and a first round of error-catching all happen on Linux.

But the only compiler that produces a working MacSurf binary is **Metrowerks CodeWarrior Pro 8**, and it runs *on* Mac OS 9. There is no Linux build of it. So the actual compile and link always happen on the Mac. (Retro68's PowerPC GCC, which we lean on heavily below, can cross-compile Classic Mac binaries from Linux — but MacSurf's project is built around CodeWarrior's C89 dialect, its prefix-file mechanism, and its resource handling, so the canonical build is CodeWarrior on-Mac. We use Retro68 as a checker, not the shipping compiler.)

The trap in this arrangement is the round-trip cost. Editing on Linux is instant; sending a file to the Mac, rebuilding, and discovering you used a `//` comment is not. So the whole Linux-side workflow is built to shrink that loop: catch as much as possible before the file leaves your desk.

## The two-pass syntax check

CodeWarrior 8 compiles in strict **C89** — no C99, no GNU extensions. That dialect is unforgiving in ways a modern developer rarely thinks about: no `//` comments, no `for (int i = ...)`, no designated initializers, variables declared only at the top of a block. The full list lives in [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas), and you should skim it once before you write much code. But you don't want to *learn* that list by shipping files to the Mac one error at a time. You want a compiler on Linux to tell you.

We use two passes, because each one catches a different class of problem.

### Pass 1 — Retro68 PowerPC GCC (the closest thing to CodeWarrior on Linux)

[Retro68](https://github.com/autc04/Retro68) is a modern GCC-based cross-compiler that actually targets Classic Mac PowerPC. Because it's a real PowerPC Classic toolchain, a `-std=c89 -pedantic-errors` pass through it is the best Linux-side approximation of what CodeWarrior will accept. It knows that on this platform `int32_t` is `long`, not `int` — which matters, because CodeWarrior treats `int *` and `int32_t *` as incompatible pointer types and Retro68 will flag the same mismatch. It catches C89 violations, undefined identifiers, and a good chunk of the header tangles before they cost you a Mac round-trip.

A syntax-only check on a single changed file looks like this:

```bash
/path/to/Retro68/toolchain/bin/powerpc-apple-macos-gcc \
  -std=c89 -pedantic-errors -Wall -Wno-long-long -Wno-pedantic \
  -fsyntax-only -Dinline= -D__MACOS9__ -DCSS_INTERNAL \
  -I<shim and source include paths> \
  path/to/changed_file.c
```

A few of those flags are load-bearing and worth understanding:

- `-fsyntax-only` means "parse and type-check, don't emit an object file." We only want the diagnostics.
- `-Dinline=` defines the `inline` keyword to nothing. C89 has no `inline`, and CodeWarrior rejects it; defining it away lets shared NetSurf headers parse cleanly without our editing them.
- `-Wno-long-long` quiets warnings about `long long`, which appears in upstream headers. (Note: `long long` *exists* on CodeWarrior PPC but is **miscompiled** for multiply-by-constant — see [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas). The pre-flight check can't catch a codegen bug; it only catches syntax.)
- `-D__MACOS9__` and `-DCSS_INTERNAL` match defines the project sets, so the right code paths are visible to the parser.

One thing to make peace with: this check is **not** expected to come back perfectly clean. The macos9 shim headers (next section) deliberately diverge from the real NetSurf library headers, so you'll see pre-existing errors from that mismatch that CodeWarrior, with its own access-path ordering, resolves fine. What you're watching for is **new** errors at the lines you just edited. If your change introduced them, fix them now. If they were there before you touched the file, leave them.

> **TODO (verify):** The exact `-I` include-path set varies by which subtree the file lives in (frontend, libcss, libdom, etc.). The maintainer's pre-flight invocation assembles these per-file; a single canonical list isn't checked into the repo. Confirm the current set against the build notes before relying on a fixed copy-paste.

### Pass 2 — plain `gcc -std=c89 -pedantic` against the shim include paths

The second pass uses your system's ordinary `gcc` (or `clang`) in C89 mode, pointed at the macos9 shim include directories:

```bash
gcc -fsyntax-only -std=c89 -pedantic -Dinline= \
  -Ibrowser/netsurf/frontends/macos9/shims \
  -Ibrowser/netsurf/frontends \
  -Ibrowser/netsurf/include \
  -Ibrowser/netsurf \
  -include stdbool.h \
  path/to/changed_file.c
```

Why a second compiler when Retro68 already exists? Two reasons. Plain GCC is everywhere — you don't need the Retro68 toolchain installed to do a quick sanity check — and a second independent front-end sometimes flags a portability issue the first one waved through. The `-include stdbool.h` forces a definition of `bool`/`true`/`false` so headers that assume it compile, mirroring how `macsurf_prefix.h` pulls in `<MacTypes.h>` first on the Mac.

Neither pass is a substitute for the real CodeWarrior build — they can't see CodeWarrior's exact header resolution, its `long long` miscompile, or anything that only shows up at link or runtime. They're a filter, and a good one: most of the trivial C89 mistakes die here instead of costing you a trip to the Mac.

## The shim layer: making POSIX-shaped code compile on a system that isn't POSIX

NetSurf was written for Unix-like platforms. Its core and its libraries reach for things Mac OS 9 simply doesn't have: POSIX file descriptors, `<iconv.h>`, `<sys/time.h>`, parts of the C library that classic Mac runtimes never shipped. Rather than fork every library to delete those references, MacSurf provides a **shim layer** — a set of stand-in headers and source files that satisfy the `#include`s and give the calls somewhere to land on the Mac Toolbox.

It comes in two flavors, and the distinction matters when you're reading the tree.

**POSIX stubs** live in `browser/netsurf/frontends/macos9/shims/`. These implement (or fake) the Unix-flavored functionality the engine expects. You'll find `iconv.h`/`mac_iconv.c` (character-set conversion routed through the Mac's facilities), `dirent.h`/`mac_dirent.c` (directory iteration), `stat.h`/`mac_stat.c`, `mac_file_io.c` and `mac_time.c`, plus standalone `stdbool.h`, `stdint.h`, `inttypes.h`, and a `sys/` subdirectory holding `types.h` and `select.h`. (The `sys/time.h` stub isn't here — it lives one level up, in the frontend's own `sys/` directory, not the shims one.) Some are real reimplementations on top of the Toolbox; others are minimal stubs that exist purely so a translation unit compiles.

**Stub headers** sit alongside them in the frontend directory (and in subfolders like `nsutils/`). These shadow external dependencies that aren't available on OS 9 — for example the `libwapcaplet`, `nsutils`, and various `sys/` headers — so the rest of the code finds a header where it expects one. This is the layer the syntax-check passes above point their `-I` at, and it's why the Retro68 pass isn't expected to come back clean: the shim headers are intentionally not byte-identical to the real library headers they stand in for.

There's a sharp edge here worth calling out, because it has bitten this project more than once. **A stub header must use the same include guard as the real header it shadows.** If the stub picks its own guard and the real header uses a different one, CodeWarrior's access-path search can pull *both* into the same translation unit — the stub processes first and sets its guard, then later the real file is found by a different path, its own guard still unset, and it processes too. The result is a flood of "illegal name overloading" errors as every shared type gets defined twice. Matching the guards is what keeps exactly one of the two in play. That story, and several siblings like it, are in [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas).

The payoff of the shim layer is that the NetSurf core and its five ported libraries (`libcss`, `libdom`, `libhubbub`, `libparserutils`, `libwapcaplet`) compile largely unmodified — the platform-specific divergence is concentrated in one place instead of smeared across 800-plus files. When you add a new file that reaches for a POSIX facility, your first question should be "is there already a shim for this?" before you write a new one. The architecture is mapped in the [Architecture Overview](Architecture-Overview).

## Line endings: the one rule you cannot forget

This one is small, invisible, and will waste an afternoon if you miss it. **Every C, header, and Rez source file destined for the Mac needs classic Mac CR (`\r`) line endings.** Not Unix LF (`\n`), not Windows CRLF — bare carriage returns.

Classic Mac OS used CR as its line terminator, where Unix used LF and DOS/Windows used CRLF. CodeWarrior's editor and compiler expect the Mac convention, and a file full of Unix LFs can confuse the toolchain's line handling in ways that produce baffling errors far from the real cause. Your Linux editor, naturally, saves LF. So the conversion happens as part of moving a file across.

Convert a file in place like this:

```bash
# LF -> CR (strip the LF after turning each newline into CR)
sed 's/$/\r/' input.c | tr -d '\n' > output.c
```

That one-liner is the canonical method we use, and it needs nothing beyond the tools already on a Linux box. If you'd rather have a dedicated converter, the `dos2unix` package ships a `unix2mac` command that does the same LF-to-CR job — but check that your distro's `dos2unix` actually includes it before you reach for it, since the `sed`/`tr` pipe always works.

This rule is specific to **source going to the Mac**. It does *not* apply to these wiki pages, your Linux-side scratch files, or anything that stays on Linux — those keep ordinary LF. The CR requirement is a one-way gate at the border crossing.

## Moving files to the Mac

> **On MacSurf's own bench this is fully automated.** A single command pushes
> the sources, builds them in CodeWarrior, launches the app, stuffs it, and
> drops the archive on the file server — driving the Mac's GUI apps over SSH
> with AppleScript. See [The Automated Ship Pipeline](automated-ship.md). The
> transfer methods below are the general background you need to understand
> *why* that pipeline is built the way it is (particularly the fork problem,
> which is why it copies with `ditto --rsrc` and packages with StuffIt), and
> they remain the right starting point for a different setup.

So you've edited, syntax-checked, and converted line endings. Now the file has to physically reach the Power Mac. Exactly how depends on your setup — a real Mac on your bench, an emulator, a Mac on the same network — but the underlying problem is always the same, and it's worth understanding before you pick a method.

### The fork problem (read this first)

A classic Mac file is not a flat stream of bytes. It can carry **two forks**: a *data fork* (ordinary unstructured data, like any file you know) and a *resource fork* (structured records — icons, UI definitions, and on classic Mac OS the application's executable machine code itself).<sup>[1]</sup> Resource forks are native only to Apple filesystems (HFS and HFS Plus on this era's hardware); they have no place to live on FAT, on a plain USB stick, or on a vanilla network share.<sup>[1]</sup>

For *source code* you're transferring, this matters less than you'd think — a `.c` file is all data fork, no resources, so it survives a naive copy. But the moment you're moving a built application, an icon file, or anything with a resource fork, a careless copy through FAT or a non-Apple share silently drops the resource fork and leaves you with a dead file. The classic, baffling symptom: an app you copied to a USB stick won't launch, because the executable code *was* the resource fork and it's gone.<sup>[1]</sup> So the transfer methods below all exist, in one way or another, to preserve forks across that border.

### File sharing over the network (AFP / netatalk)

The most ergonomic option if your Mac and your Linux box are on the same network is a network file share, so the Mac sees a folder it can copy from directly. **[netatalk](https://netatalk.io/)** is an open-source implementation of AFP (Apple Filing Protocol) for Linux and other Unix-like hosts; it preserves Mac metadata using filesystem attributes or AppleDouble sidecar files.<sup>[2]</sup> From the Mac you connect through **Chooser → AppleShare → "Server IP Address…"** and enter the Linux box's IP.<sup>[2]</sup>

Two version notes save real grief here. Mac OS 8.1 and later speak AFP over TCP/IP out of the box, so connecting by IP address is the reliable path on OS 9.<sup>[2]</sup> And the netatalk 3.x series **dropped** classic AppleTalk file sharing over ethernet — so if you're trying to reach an OS 9 Mac, use the 2.x branch (or 4.x, which re-added it), and prefer AFP-over-TCP regardless.<sup>[3]</sup>

### Disk images

A disk image holds an entire HFS or HFS+ volume in a single file, so it preserves forks intact by definition — the forks live inside a real Mac filesystem that just happens to be packaged as one file. On OS 9 you mount images with Apple's **Disk Copy**; on Linux, `hfsprogs`/`hfsutils` and FUSE drivers can read and write HFS/HFS+, and `genisoimage` can build HFS images.<sup>[4]</sup> Dropping your files into an image on Linux and mounting that image on the Mac avoids any fork loss on the hop. This is also handy under emulation, where a shared image is often the simplest channel into the guest OS.

### StuffIt archives (`.sit`)

**StuffIt** was the dominant Mac compressor from the late 1980s through the Mac OS X era, and a `.sit` archive **saves the resource forks** of the files inside it — which is exactly what let Mac files survive a stint on non-Mac storage.<sup>[5]</sup> On the Mac, **StuffIt Expander** unpacks them. (A `.sea` file is a *self-extracting* StuffIt archive — really a small program — so it only unpacks on a Mac, not on Linux without StuffIt tooling.<sup>[5]</sup>) StuffIt is also how MacSurf's recommended build artifact ships: the supported way to get a buildable source tree onto the Mac is the StuffIt build pack, not a fresh `git clone`. See [Building MacSurf](Building-MacSurf) for that path.

### BinHex (`.hqx`) and MacBinary (`.bin`)

These are encodings rather than network protocols, useful when your transfer channel is text-only or single-fork. **BinHex 4.0** (`.hqx`) packs both forks and the Finder metadata into a single **7-bit ASCII** text stream, which is why it survived email and ASCII-only paths for decades.<sup>[6]</sup> It's worth knowing that BinHex is *encoding*, not *compression* — an `.hqx` file is bigger than the original; its job is 7-bit safety.<sup>[6]</sup> **MacBinary** (`.bin`) does the same fork-preservation job with a 128-byte header in front of the two forks.<sup>[6]</sup> For day-to-day source transfer you'll rarely reach for these, but they're the right tool when your only pipe is a text channel.

### A note on the `._` sidecar files

If you transfer through a method that uses **AppleDouble**, you'll see companion files named with a leading `._` — `foo.c` getting a `._foo.c` next to it.<sup>[7]</sup> These aren't junk you accidentally created. AppleDouble splits a Mac file in two: the data fork keeps the original name (so Unix tools can edit it), and the `._`-prefixed companion holds the resource fork and Finder info.<sup>[7]</sup> Delete the sidecar and you strip the resource fork off the real file. Leave them alone.

## After the file lands

Getting the file onto the Mac is only the first half. (On the automated
pipeline, everything in this section down to the rebuild happens without you —
but adding a *new* `.c` file to the CodeWarrior project is still manual, and
always will be.) On the Mac side, the file gets unpacked into the source tree, and if you've added a *new* `.c` file it has to be added to the CodeWarrior project by hand — CodeWarrior does not auto-discover source files. Before rebuilding after any file change, the project needs **"Remove Object Code"** run first so CodeWarrior recompiles everything cleanly; a header-only edit in particular won't trigger a rebuild of the `.c` files that include it unless you force it. The mechanics of the Mac-side build, the project file list, and the access-path structure all live in [CodeWarrior Project Settings](CodeWarrior-Project-Settings) and [Building MacSurf](Building-MacSurf).

When something behaves wrong after a transfer, resist the urge to blame the transfer. A correctly converted, correctly copied file is what it is; if the symptom looks like staleness, the real cause is almost always in the code — a missing include, a mismatched guard, a wrong field path — and that's where the time is best spent. [Diagnostics & Debugging](Diagnostics-and-Debugging) covers the file-backed log and the crash-reading techniques that find those real causes, and [Contributing & Expanding](Contributing-and-Expanding) covers the regression-audit habits that keep a change from quietly breaking something three files away.

## Sources

1. Resource fork (data/resource forks; loss on non-Apple filesystems) — https://en.wikipedia.org/wiki/Resource_fork
2. netatalk — AFP server for Unix; Chooser → AppleShare connection; AFP-over-TCP on Mac OS 8.1+ — https://netatalk.io/ and https://netatalk.io/docs/Connect-to-AFP-Server
3. netatalk 3.x dropping classic AppleTalk; use 2.x/4.x for OS 9 — https://marmanold.com/retro/linux-fileshare-for-classic-macintoshes/
4. Disk images preserve forks; Disk Copy on OS 9; HFS tooling on Linux — https://www.gryphel.com/c/image/ and https://www.macintoshrepository.org/articles/75-how-to-mount-a-disk-image-under-mac-os-7-8-or-9
5. StuffIt (`.sit` saves resource forks; `.sea` self-extracting; StuffIt Expander) — https://en.wikipedia.org/wiki/StuffIt
6. BinHex 4.0 / MacBinary (7-bit ASCII encoding; encoding not compression) — https://en.wikipedia.org/wiki/BinHex
7. AppleSingle/AppleDouble (`._` sidecar carries the resource fork) — https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats
8. Retro68 PowerPC/Carbon cross-compiler — https://github.com/autc04/Retro68
