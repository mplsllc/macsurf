# Classic Mac OS Application Development Fundamentals

Background for developers coming from modern toolchains who need to understand the
Classic Mac OS programming model that MacSurf targets (Mac OS 9, PowerPC, Carbon).
All claims below were checked against the cited source; where a fact could not be
fully verified it is flagged as such.

## The Macintosh Toolbox

- The Macintosh Toolbox is the collection of system-level routines and APIs (originally in ROM) that provided the GUI, event handling, resource management, and hardware abstraction for Classic Macs from 1984 through Mac OS 9; the earliest ROMs were hand-optimized 68000 assembly compressed into 64 KB. — https://wiki.retrotechcollection.com/Macintosh_Toolbox
- The Event Manager originally used a *polling* model: the app's main loop asked for an event via `GetNextEvent`, which returned an event from the queue or returned immediately if none was waiting. — https://en.wikipedia.org/wiki/Carbon_(API)

## Cooperative multitasking and WaitNextEvent

- Classic Mac OS uses *cooperative* (not preemptive) multitasking: each application must voluntarily yield the processor for another to run. — https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html
- `WaitNextEvent` (System 7+) superseded `GetNextEvent`; it fetches the next matching event, and during the wait grants time to background processes — the application yields the CPU until an event needs its attention. — https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html
- (Note: the Inside Macintosh page documenting `WaitNextEvent`'s exact signature — `eventMask`, `theEvent`, `sleep`, `mouseRgn` — returned HTTP 403 and could not be fetched; the `sleep` parameter detail is therefore unverified here, though the cooperative-yield behavior is confirmed by the sources above.)

## Carbon vs. the classic Toolbox

- Carbon is a C API that lets a single source base run on both Classic Mac OS and Mac OS X; it was introduced in incomplete form in 2000 (as a shared library compatible with Mac OS 8.1) and gained full Mac OS X support in 2001 with Mac OS X 10.0. — https://en.wikipedia.org/wiki/Carbon_(API)
- The Carbon Event Manager replaces the polling loop: the developer registers event handlers and enters the event loop, and Carbon dispatches events to the app (built on Core Foundation's `CFRunLoop`), eliminating busy-waiting. — https://en.wikipedia.org/wiki/Carbon_(API)
- In the classic Toolbox many data structures were exposed and manipulated by direct field access; in Carbon most are *opaque*, manipulated through accessor functions, and Carbon apps could no longer install interrupt handlers or device drivers. — https://en.wikipedia.org/wiki/Carbon_(API)
- Secondary (community) sources state Carbon supports roughly 70% of the original Toolbox calls — treat the exact figure as approximate folklore, not an Apple-stated number. — https://en.wikipedia.org/wiki/Carbon_(API)

## Open Transport (classic TCP/IP)

- Open Transport is Apple's implementation of the System V STREAMS networking stack, based on code licensed from Mentat's Portable Streams; it provided a single API over both TCP/IP and AppleTalk and supported loading/unloading protocol modules on demand. — https://en.wikipedia.org/wiki/Open_Transport
- It was introduced in May 1995 with the Power Mac 9500 and System 7.5.2, replacing the older MacTCP (which ran under emulation and was slow on PowerPC); from Mac OS 8.0 onward Open Transport was built into the OS. — https://en.wikipedia.org/wiki/Open_Transport

## MixedMode, UPPs, and routine descriptors

- A Universal Procedure Pointer (UPP), also called a routine descriptor, is a small data structure describing the calling conventions and RAM location of a function; it begins with a 68000 instruction so it can be called like a function pointer from 68k code. — https://orangejuiceliberationfront.com/universal-procedure-pointers/
- The Mixed Mode Manager handles switching between native PowerPC execution and 68000 emulation; `CallUniversalProc` inspects the UPP and, if the target is already PowerPC, jumps straight to it without starting the 68k emulator. UPPs could be "fat," holding both PowerPC and 68000 code, with the manager jumping to the right offset for the running architecture. — https://orangejuiceliberationfront.com/universal-procedure-pointers/
- Developers created UPPs with `NewXXXUPP`-style calls and had to manage their memory; on the Intel transition Apple dropped mixing of PowerPC and Intel code entirely because the PowerPC's transparent endian-switch had no Intel equivalent. — https://mjtsai.com/blog/2013/03/31/universal-procedure-pointers/

## Resource forks

- Classic Mac files have two forks: a *data fork* (unstructured bytes, random access) and a *resource fork* (structured records accessed database-style); native filesystems MFS, HFS, and HFS Plus support both. — https://en.wikipedia.org/wiki/Resource_fork
- The Resource Manager loads resources by type, ID, or name and keeps open forks in a stack (document → system), so local resources override global ones. Each resource has a four-byte OSType, a signed 16-bit ID, and an optional name; common types include `ICON`/`PICT`, `MENU`/`MBAR`, `DLOG`/`DITL`, `snd `, and (68k-era) `CODE`. — https://en.wikipedia.org/wiki/Resource_fork
- On non-Mac filesystems (FAT, SMB, NFS), macOS uses AppleDouble: the data fork is a normal file and the resource fork + metadata go in a hidden `._`-prefixed companion file. — https://en.wikipedia.org/wiki/Resource_fork

## Type and creator codes

- A type code is a four-byte OSType describing the file kind (e.g. `TEXT`, `PICT`); applications themselves have type code `APPL`. The creator code is a four-byte OSType identifying the originating application, letting the Finder open a document with its parent app — richer than a filename extension. — https://en.wikipedia.org/wiki/Creator_code , https://vintagemacmuseum.com/macintosh-type-and-creator-codes/
- Apple maintained a registry of creator codes to avoid collisions and reserved all-lowercase codes for itself; the application's `BNDL`/`FREF` resources bind icons and document types. Creator codes have been ignored since Mac OS X Snow Leopard, superseded by Uniform Type Identifiers. — https://en.wikipedia.org/wiki/Creator_code
- (Relevant to MacSurf: type/creator codes are case-sensitive four-char values — MacSurf's creator is uppercase `MPLS`. The vintagemacmuseum source confirms the four-char format but did not explicitly address case-sensitivity; the case-sensitivity point comes from the project's own build notes, not the cited web sources.)

## Toolchains: MPW and Retro68

- Macintosh Programmer's Workshop (MPW) was Apple's official development environment for Classic Mac OS; development began late 1984, MPW 1.0 shipped 24 Sept 1986, and it provided a Unix-shell-like "worksheet" with 68k/PowerPC assemblers and Pascal/C/C++ compilers. It was later made a free download after CodeWarrior superseded it, and was deprecated by Xcode in 2003. — https://en.wikipedia.org/wiki/Macintosh_Programmer's_Workshop
- Retro68 (github.com/autc04/Retro68) is a modern GCC-based cross-compiler for 68K and PowerPC (including Carbon) Classic Macs, bundling binutils 2.39, GCC 12.2.0, and newlib 4.2. It ships a `Rez` resource compiler plus `MakePEF`/`MakeImport` for PowerPC PEF executables, and includes open-source "Multiversal Interfaces" (optionally Apple's Universal Interfaces 3.x). It targets C++17. — https://github.com/autc04/Retro68/blob/master/README.md
- (Note: MacSurf itself builds with CodeWarrior 8, not Retro68; Retro68's PPC GCC is used as a Linux-side syntax pre-flight check per the project's build notes.)

## Beginner gotchas / things that surprise people

- **No preemption.** A long computation that never calls `WaitNextEvent` (or otherwise yields) freezes the *whole machine*, not just your app. Cooperative scheduling is the rule. — https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html
- **A "file" is two forks.** Copying a Mac file through a Unix/Windows filesystem can silently strip the resource fork (icons, code, UI resources) unless AppleDouble/MacBinary preserves it. — https://en.wikipedia.org/wiki/Resource_fork
- **Function pointers aren't always callable directly.** On the 68k→PowerPC boundary you pass UPPs/routine descriptors to system callbacks, not bare pointers; getting this wrong yields crashes inside the Mixed Mode Manager. — https://orangejuiceliberationfront.com/universal-procedure-pointers/
- **Carbon structs are opaque.** Code that worked by poking struct fields in the classic Toolbox must move to accessor functions under Carbon. — https://en.wikipedia.org/wiki/Carbon_(API)
- **File identity isn't the extension.** The Finder keys off type/creator codes, so a correctly-typed file with no extension still opens in the right app — and a wrong creator code orphans the document. — https://en.wikipedia.org/wiki/Creator_code

## Sources

- https://wiki.retrotechcollection.com/Macintosh_Toolbox
- https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html
- https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html
- https://en.wikipedia.org/wiki/Carbon_(API)
- https://en.wikipedia.org/wiki/Open_Transport
- https://orangejuiceliberationfront.com/universal-procedure-pointers/
- https://mjtsai.com/blog/2013/03/31/universal-procedure-pointers/
- https://en.wikipedia.org/wiki/Resource_fork
- https://en.wikipedia.org/wiki/Creator_code
- https://vintagemacmuseum.com/macintosh-type-and-creator-codes/
- https://en.wikipedia.org/wiki/Macintosh_Programmer's_Workshop
- https://github.com/autc04/Retro68/blob/master/README.md
