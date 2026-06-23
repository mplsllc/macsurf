# Resources & Prior Art

A jumping-off page for anyone working on MacSurf or starting their own Classic Mac OS project. Everything here is a link the maintainer actually leans on, grouped by what you'd reach for it. The notes say *why* each one earns its place — that's the part that saves you an afternoon of dead ends.

MacSurf didn't appear from nothing. It's a fork of an existing engine, it learned its networking from a couple of open-source projects that solved the same problems first, and it's built with a toolchain that's been kept alive by a small, stubborn archival community. Knowing where those pieces live — and which prior art is worth reading versus which is a dead end — is half the work.

---

## The engine: NetSurf

MacSurf is a port of NetSurf, so the upstream project is the single most useful external reference for how the browser core behaves. If you want to understand the rendering pipeline or the library boundaries, start here. (For how MacSurf wires the Mac frontend onto it, see [Architecture Overview](Architecture-Overview).)

- **[NetSurf project site](https://www.netsurf-browser.org/)** — the home of the engine. NetSurf is a from-scratch C browser (not a WebKit/Gecko shell), GPLv2-licensed, built to be lightweight and portable. That portability is exactly why it ports to a 25-year-old Mac at all. Source: <https://www.netsurf-browser.org/about/>
- **[NetSurf sub-projects / libraries](https://www.netsurf-browser.org/projects/)** — the modular pieces. The engine deliberately splits parsing and styling into standalone, separately-versioned libraries.
- **[netsurf-browser on GitHub](https://github.com/netsurf-browser)** — the source for the core libraries MacSurf ports: [libcss](https://github.com/netsurf-browser/libcss) (CSS parse + cascade), [libdom](https://github.com/netsurf-browser/libdom) (W3C DOM in C), [libhubbub](https://github.com/netsurf-browser/libhubbub) (HTML5 parser), libparserutils, and [libwapcaplet](https://github.com/netsurf-browser/libwapcaplet) (string interning). The five core libraries are MIT-licensed even though the application is GPLv2 — that split is what makes embedding them practical. When you hit a libcss quirk on the Mac, the upstream source is where you confirm the behavior is real and not a port artifact.

NetSurf's **RISC OS and AmigaOS frontends** are the closest architectural cousins to MacSurf's Mac frontend, because both solved cooperative multitasking on a non-POSIX system — the same problem OS 9 hands you. They live under `frontends/riscos/` and `frontends/amiga/` in the NetSurf tree and are worth reading before you touch frontend code. More than once we've found a feature already solved in core or in one of those frontends and saved ourselves from reinventing it.

---

## Open Transport & networking references

MacSurf does its own TCP/IP and HTTPS on the Mac (see [Networking & TLS](Networking-and-TLS)). Open Transport — Apple's classic networking stack — is sparsely documented and full of sharp edges, so working open-source clients are gold. These are the ones that taught us the cooperative-yield-plus-OT pattern.

- **[Classilla](https://sourceforge.net/projects/classilla/)** — a Mozilla-era browser still maintained for Classic Mac OS. Its `macsockotpt.c` (the NSPR sockets layer over Open Transport) and the standalone TCP-over-OT code in its LDAP libraries are the most complete real-world OT networking reference we know of. It's a full Carbon browser that runs on OS 9 today, which also makes it a useful sanity check: if Classilla can fetch a page, the platform can.
- **[cy384/ssheven](https://github.com/cy384/ssheven)** — a modern, production SSH client for Mac OS 9. It does the cooperative-thread-plus-Open-Transport dance correctly and cleanly, and it's small enough to read end to end. This is the best single example of "how to do blocking network I/O on a cooperative OS without freezing the machine."
- **[cy384/miscellany](https://github.com/cy384/miscellany)** — its `retro68-demos/ot-tcp-demo.c` is the shortest known-good OT HTTP client (around 220 lines, adapted from Apple's own `OTSimpleDownloadHTTP.c` sample). When you want to see the bare bones of an OT endpoint open/connect/send/receive without a browser wrapped around it, start here.

A caveat worth stating plainly: MacSurf's current Carbon build uses the `*InContext` Open Transport variants and an OT client context set up at startup, which is *not* exactly how the plain-OT references above initialize. Read them for the connect/yield/teardown pattern and the endpoint lifecycle, not as a line-for-line template for the Carbon path. The reasoning behind the `*InContext` choice is in [Networking & TLS](Networking-and-TLS).

For background on what Open Transport even is — Apple's STREAMS-based stack that replaced MacTCP — see <https://en.wikipedia.org/wiki/Open_Transport>.

---

## Building it: CodeWarrior Pro 8 & CarbonLib

MacSurf is compiled on real Mac OS 9 with **Metrowerks CodeWarrior Pro 8**. It's long out of print, but the archival community keeps it available. The full install-and-configure story is in [Setting Up the Build Environment](Setting-Up-the-Build-Environment) and [CodeWarrior Project Settings](CodeWarrior-Project-Settings); these are where you *get* the bits.

- **[CodeWarrior Pro 8.x on Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x)** — the base disc image plus the 8.1 / 8.2 / 8.3 updaters, with system requirements and install notes. The updaters are cumulative and you apply them in order (8.0 → 8.2 → 8.3; the 8.1 step is optional because 8.2 accepts an 8.0 base, but 8.3 won't drop onto a bare 8.0). Source: <https://macintoshgarden.org/apps/codewarrior-pro-8x>
- **[CodeWarrior Pro 8.x on Macintosh Repository](https://www.macintoshrepository.org/1351-codewarrior-pro-8-x)** — a second source for the same files plus community install lore (the `MetroNub` extension, dropping the `Metrowerks` folder into Preferences). Handy if a Garden mirror is slow.
- **CarbonLib** — the shared library that makes a single Carbon binary run on both OS 9 and early OS X. MacSurf needs **CarbonLib 1.5+** to run and **1.6+** is recommended to build. CarbonLib shipped with later OS 9 updates and was also distributed by Apple as a standalone installer. If your OS 9 install is missing it or is too old, you'll need to add it before MacSurf will launch — without CarbonLib fully engaged, the app's networking calls crash at startup. (CarbonLib downloads now live in the same community archives as the OS itself; Apple's original download pages are long gone.)

You can get it from **[CarbonLib on Macintosh Garden](https://macintoshgarden.org/apps/carbonlib)**, which hosts Apple's original installers for 1.5, 1.6, and 1.6.1 (each a `CarbonLib_x.x.smi` disk image, with multiple mirrors and MD5 checksums). Grab **1.6** (or 1.6.1) to build against; 1.5 is the minimum to run. One compatibility note from that page: CarbonLib 1.3.1 and up need Mac OS 8.6+, which is moot on OS 9.

- **[StuffIt Expander 5.5 on Macintosh Garden](https://macintoshgarden.org/apps/stuffit-expander-55)** — you'll need StuffIt Expander to unpack nearly everything above: the MacSurf build pack (`.sit`), and the CodeWarrior and CarbonLib downloads, which arrive as compressed `.smi` / `.bin` disk images. OS 9 usually ships with a copy, but if yours doesn't have one — or has one too old to open a download — **version 5.5 is the easiest to get running on a bare system**, and once it's in place it expands the rest. Grab it first.

For the executable format CodeWarrior produces (PEF / CFM) and why the Code Fragment Manager matters on PowerPC, see <https://en.wikipedia.org/wiki/Preferred_Executable_Format>.

---

## Utilities: debugging & screen capture

Two more tools the maintainer keeps installed on the build machine. Neither is required to *build* MacSurf, but both earn their keep once you're debugging or filing reports.

- **[MacsBug on Macintosh Garden](https://macintoshgarden.org/apps/macsbug)** — Apple's classic low-level assembly debugger. It installs into the System Folder and drops you into a text-mode debugger when an unhandled exception fires, so you can read the program counter, the registers, and a stack crawl at the moment of a crash. It's the tool behind every "we need a `wh` stack capture" request. How to use it — and the keyboard-access caveat on USB-only Macs — is on [Diagnostics & Debugging](Diagnostics-and-Debugging).
- **[Snapz Pro 2 on Macintosh Garden](https://macintoshgarden.org/apps/snapz-pro-2)** — a screen-capture utility for OS 9. Invaluable for grabbing a render bug, a misrendered page, or a before/after to attach to a bug report or compatibility thread; OS 9's built-in Cmd-Shift-3 capture is clumsy by comparison and awkward to get off the machine.

---

## Alternative toolchain: Retro68

CodeWarrior is the only supported compiler for shipping MacSurf, but you don't need it to *syntax-check* code, and you may want it for your own projects.

- **[Retro68](https://github.com/autc04/Retro68)** — a modern GCC-based cross-compiler for 68K and PowerPC Classic Macs, including Carbon. It bundles binutils, GCC, newlib, a `Rez` resource compiler, and `MakePEF` for producing PowerPC executables. MacSurf uses its PowerPC GCC as a Linux-side pre-flight pass — `-std=c89 -pedantic` catches a lot of CodeWarrior C89 violations before files ever reach the Mac (see [Cross-Developing from Linux](Cross-Developing-from-Linux)). It's also the natural starting point if you're building a Classic Mac project from a modern Unix host and don't want to live inside the IDE — see [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project). Source: <https://github.com/autc04/Retro68/blob/master/README.md>

---

## Emulators: SheepShaver & QEMU-PPC

You can run and smoke-test MacSurf in emulation, which is much faster to iterate against than carrying a build to physical hardware every time. Both options are covered in depth — install, configure, gotchas — in [Setting Up the Build Environment](Setting-Up-the-Build-Environment). The short version of which to pick:

- **[SheepShaver](https://sheepshaver.cebix.net/)** — a PowerPC Mac emulator. Easy to set up and fast, but it tops out at **Mac OS 9.0.4** (it doesn't emulate the MMU, so 9.1–9.2.2 won't boot) and it needs a PowerMac ROM image plus an OS 9 install you supply yourself. Great for "does the build launch and render," not a substitute for real hardware on timing- or hardware-specific bugs. Source: <https://en.wikipedia.org/wiki/SheepShaver>
- **[QEMU PowerMac (`qemu-system-ppc`)](https://www.qemu.org/docs/master/system/ppc/powermac.html)** — full PowerMac system emulation with `g3beige` and `mac99` machine types. It can run **Mac OS 9.2.2** (the version MacSurf is hardware-verified against) and needs **no Apple ROM** because it uses OpenBIOS — you still supply your own OS 9 install media. Slower to set up but reaches the real target OS version. Source: <https://www.qemu.org/docs/master/system/ppc/powermac.html>
- **[cebix/macemu](https://github.com/cebix/macemu)** — the shared source repo for SheepShaver (and Basilisk II). Go here for builds and issues.

One trap worth flagging up front: **Basilisk II is 68K-only and cannot run any of this.** MacSurf is PowerPC, OS 9 is PowerPC, CodeWarrior 8 is PowerPC — Basilisk II tops out at Mac OS 8.1 and 68K apps. Reach for SheepShaver or QEMU. Source: <https://github.com/cebix/macemu>

And the standing caveat: a clean run in any emulator is a smoke test, not proof. The emulated CarbonLib and Control Manager are more forgiving than a real G3/G4. The truth comes from hardware — see [Diagnostics & Debugging](Diagnostics-and-Debugging) for why.

---

## Software & OS archives

Where you find Mac OS 9 itself, period software, and (often) the toolchain mirrors above.

- **[Macintosh Garden](https://macintoshgarden.org/)** — the community archive for abandoned Mac software. It hosts the CodeWarrior images, and it's the kind of place MacSurf is built to browse — old shareware, fan sites, the long tail of the Mac web. A good real-world test target.
- **[Macintosh Repository](https://www.macintoshrepository.org/)** — a second large archive, overlapping with the Garden but with its own holdings and notes. Useful as a mirror and for cross-checking install instructions.

A neutral note on legality, because it comes up: Apple ROM images and Mac OS 9 install media are still Apple's copyrighted property. The low-risk path is to image install media you own. QEMU sidesteps the *ROM* question entirely with OpenBIOS, but you still supply the OS yourself.

---

## Communities

When you're stuck on something genuinely Mac-OS-9-specific — a CarbonLib quirk, an Open Transport oddity, a hardware behavior — these are where the people who remember live.

- **[68kMLA forums](https://68kmla.org/)** — the 68k Macintosh Liberation Army, despite the name the broadest active vintage-Mac community, very much including PowerPC and OS 9. Deep institutional memory on hardware, emulation, networking, and period software.
- **[E-Maculation](https://www.emaculation.com/)** — focused on Mac emulation (SheepShaver, Basilisk II, QEMU). The best practical guides for getting an emulator booting, and where the community hosts SheepShaver builds and walkthroughs. (The upstream source lives at [cebix/macemu](https://github.com/cebix/macemu); E-Maculation is where the community packages and explains it.) (Their pages can be slow or block automated fetches; visit in a browser.)

- **[MacSurf Discussions](https://github.com/mplsllc/macsurf/discussions)** — the project's own space for open-ended questions ("is anyone already working on X?", "did anyone get this site to render?") as distinct from the [issue tracker](https://github.com/mplsllc/macsurf/issues), which is for actionable, tracked work. The right place to introduce yourself or ask before diving in. (Filing bugs and features is covered in [Contributing & Expanding](Contributing-and-Expanding).)

---

## Apple & Toolbox reference documentation

The original documentation for the platform. Most of it predates the web's current shape, so expect dead links and archive.org detours — but the *Inside Macintosh* material is still the authoritative description of how the Toolbox, Carbon, Open Transport, and the Resource Manager actually work.

- **[Apple Developer Archive](https://developer.apple.com/library/archive/navigation/)** — Apple's own archive of legacy documentation. It still carries the Carbon multitasking concepts, the runtime-architectures material, and much of the classic Toolbox reference. Some pages 403 to automated tools but load fine in a browser; the [cooperative-multitasking concepts page](https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html) is a good entry point for understanding why OS 9 has no preemptive threads.
- **Inside Macintosh** — Apple's multi-volume reference for Classic Mac OS programming (the Toolbox, Files, Memory, Networking with Open Transport, and more). It's the canonical source for any Toolbox call you need to look up, and it's freely archived online. Two good mirrors: **[vintageapple.org's Inside Macintosh archive](https://vintageapple.org/inside_o/)** for the original volumes, and the **[1992–1994 Inside Macintosh series on the Internet Archive](https://archive.org/details/inside-macintosh-1992-1994)** — the later, Carbon-era set, which is the one you want for the Toolbox and Open Transport material this wiki leans on.

For the conceptual background woven through this wiki — the Toolbox, cooperative multitasking, resource forks, type/creator codes, UPPs and Mixed Mode — see [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project), which explains them in the order you'd hit them on a real project, with sources.

---

## A word on prior art that *isn't* here

A few projects come up when people search for "NetSurf on Mac OS 9" or "Classic Mac browser," and it's worth saving you the search:

- **MacSurf appears to be the first serious NetSurf port to Classic Mac OS.** The NetSurf developer list has a single 2017 "Port to OS9?" thread that went nowhere. There is no prior OS 9 NetSurf port to crib from — which is why the Amiga and RISC OS frontends are the references instead.
- **iCab** is a real OS 9 browser but is closed source, so it's no use as a code reference.
- **WaMCom** was Classilla's predecessor and shares its codebase — read Classilla instead.

---

## Sources

- NetSurf: <https://www.netsurf-browser.org/about/>, <https://www.netsurf-browser.org/projects/>, <https://github.com/netsurf-browser/libcss>, <https://github.com/netsurf-browser/libdom>, <https://github.com/netsurf-browser/libwapcaplet>
- Classilla: <https://sourceforge.net/projects/classilla/>
- Open Transport references: <https://github.com/cy384/ssheven>, <https://github.com/cy384/miscellany>, <https://en.wikipedia.org/wiki/Open_Transport>
- CodeWarrior Pro 8: <https://macintoshgarden.org/apps/codewarrior-pro-8x>, <https://www.macintoshrepository.org/1351-codewarrior-pro-8-x>
- PEF/CFM: <https://en.wikipedia.org/wiki/Preferred_Executable_Format>
- Retro68: <https://github.com/autc04/Retro68>
- Emulators: <https://sheepshaver.cebix.net/>, <https://en.wikipedia.org/wiki/SheepShaver>, <https://www.qemu.org/docs/master/system/ppc/powermac.html>, <https://github.com/cebix/macemu>
- Archives & communities: <https://macintoshgarden.org/>, <https://www.macintoshrepository.org/>, <https://68kmla.org/>, <https://www.emaculation.com/>
- Apple documentation: <https://developer.apple.com/library/archive/navigation/>, <https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html>
