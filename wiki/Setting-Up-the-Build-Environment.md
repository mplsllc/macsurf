# Setting Up the Build Environment

Before you can build MacSurf, you need a working Mac OS 9 system with Metrowerks CodeWarrior Pro 8 and CarbonLib installed. This page walks you through getting that environment standing — on a real Power Mac, or on an emulator if you don't have the hardware. It's written for a developer who has never touched OS 9 or CodeWarrior; the platform-specific pieces are explained as they come up.

The short version: a Power Mac G3 or G4 running OS 9.1–9.2.2 is the real thing and the only place you can trust a build. An emulator gets you a working compiler and a way to smoke-test that the app launches and renders, which is genuinely useful — but it is not a stand-in for hardware when you're chasing a crash. We'll cover both, and be honest about where each one ends.

Once your environment is up, head to [Building MacSurf](Building-MacSurf) for the actual build, and [CodeWarrior Project Settings](CodeWarrior-Project-Settings) for the exact target configuration.

## The three pieces you need

Whichever route you take, you're assembling the same toolchain:

1. **Mac OS 9** (9.1 through 9.2.2) — the operating system you build and run on.
2. **CodeWarrior Pro 8** — Metrowerks' IDE and PowerPC compiler. This is what turns the C source into a PowerPC application. You install the base 8.0 release and then layer the 8.1, 8.2, and 8.3 updaters on top.
3. **CarbonLib 1.6+** — the shared system extension that implements Apple's Carbon API on classic Mac OS. MacSurf is a Carbon app, so it binds to CarbonLib at runtime, and CodeWarrior links against a CarbonLib stub at build time. CarbonLib ships with every copy of OS 9, so this step is usually an *update* rather than a fresh install.

You'll also want **StuffIt Expander** on hand — it's what unpacks the downloads, since the build pack and the CodeWarrior and CarbonLib disk images all arrive compressed. OS 9 normally includes it; if yours doesn't, or its copy is too old to open a download, [StuffIt Expander 5.5](https://macintoshgarden.org/apps/stuffit-expander-55) is the easiest version to get onto a bare machine, and it'll expand everything else. (Download links for CodeWarrior, CarbonLib, and StuffIt are collected on [Resources & Prior Art](Resources-and-Prior-Art).)

A word on Carbon, since it shapes everything. Carbon is a subset of the old Mac Toolbox APIs, reworked by Apple so a single binary can run on both Mac OS 8/9 and Mac OS X. On OS 9 the whole Carbon implementation lives in one extension called `CarbonLib`. MacSurf is built as a Carbon app specifically so it runs cleanly on OS 9 today and could run on early OS X without changes. That's why CarbonLib matters as much as the compiler.

> Sources for the Carbon facts: Apple's *Carbon Porting Guide* (Legacy, 2002), https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

## Route A: Real hardware

This is the supported, trustworthy path. If you want to do serious work on MacSurf — especially anything touching the event loop, scrolling, networking, or input devices — you want a real machine.

### Pick a machine

Any Power Mac that boots OS 9 natively will do. In practice that means:

- **Any Power Mac G3** (Beige, Blue & White) boots OS 9. ([everymac](https://everymac.com/systems/apple/powermac_g3/faq/power-mac-g3-boot-mac-os-9-x-support.html))
- **Most Power Mac G4s** boot OS 9 — *except* the FireWire 800 models (the 1.0 GHz, 1.25 GHz dual, and 1.42 GHz dual FW800 variants), which only boot OS X and run OS 9 apps in Classic. Check the exact model, not just "it's a G4," because they look alike. The last Mac that boots OS 9 natively is the Mirrored Drive Door (MDD) G4 1.25 from 2003. ([everymac](https://everymac.com/systems/apple/powermac_g4/faq/power-mac-g4-boot-macos-9-run-classic-applications.html))
- **G5s cannot boot OS 9 at all.** ([Wikipedia: Mac OS 9](https://en.wikipedia.org/wiki/Mac_OS_9))

MacSurf's own development machine is a G3 iMac running OS 9.2.2, and the community target is a Power Mac G4 on 9.2.2 — the two most common active OS 9 setups. CodeWarrior Pro 8 wants a G3 or better with at least 64 MB of RAM on OS 9 and around 700 MB of disk for a full install. ([Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x)) More RAM is better; you'll be running a large compile.

### Storage, if you're reviving an old machine

The internal disk on these Macs is IDE/PATA. A CF-to-IDE card or a 2.5" IDE/PATA SSD adapter drops in passively and saves you from a 25-year-old spinning drive. One catch: Power Macs older than the Quicksilver G4 (mid-2001) can only address about 128 GB on the internal IDE bus, so a larger drive will be truncated unless you add a third-party PCI IDE/SATA card. A modest SSD or CF card sidesteps that ceiling by being small enough. ([Low End Mac: SATA/SSD](https://lowendmac.com/2010/sata-and-ssd-options-for-g3-and-g4-power-macs/), [max drive size](https://lowendmac.com/2014/maximum-hard-drive-size/))

### Install OS 9.1–9.2.2

The thing that trips up first-timers: **9.2.2 is not a clean-install disc, it's an updater.** Apple shipped 9.2.1 and 9.2.2 as free updaters that require an existing 9.1+ install on the target drive. So the path is: install a full OS 9 (from a retail or restore CD, or a 9.1 install), then run the 9.2.2 updater on top. ([Macintosh Repository](https://www.macintoshrepository.org/2605-mac-os-9-0-4-9-1-9-2-1-9-2-2-international-english-updaters), [Low End Mac](https://lowendmac.com/2013/low-end-macs-compleat-guide-to-mac-os-9/))

Mac OS 9.2.2 (December 2001) is the final release of classic Mac OS and supports only G3 and G4 processors. ([Wikipedia: Mac OS 9](https://en.wikipedia.org/wiki/Mac_OS_9))

Why 9.1 at minimum? MacSurf targets 9.1–9.2.2, and font rendering in particular is noticeably crisper on 9.2.2 — QuickDraw's anti-aliased text path improved between 9.1 and 9.2. If you're seeing rough text on a 9.1 machine, that's the OS, not MacSurf. 9.2.2 is the recommended target.

### Networking and file transfer

G3/G4 Power Macs have built-in Ethernet. OS 9's TCP/IP control panel can pull an address over DHCP, so the machine joins your LAN like any other client. For wireless, an Ethernet-to-Wi-Fi bridge presents itself as plain Ethernet to the Mac — you configure it through its own web UI and the Mac never knows the difference. ([Macworld](https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html))

You'll need a way to move files from your modern machine to the Mac — both the build pack or source tree going in, and the occasional log file coming back out. Common approaches, roughly in order of how pleasant they are:

- **AFP / AppleShare** to a NAS or a netatalk-based server gives you fast, drive-like transfers over Ethernet through OS 9's Network Browser, and it preserves Mac metadata (type/creator codes and resource forks). This is the nicest option if you can set it up. ([mac-classic.com](https://mac-classic.com/articles/remote-file-storage-transfers-and-backups/))
- **FTP** over Ethernet: run an FTP server on the modern machine and use an OS 9 client like Fetch or Transmit.
- **USB thumb drive**: works, but the ports are USB 1.1 (slow), and a plain FAT-formatted drive will strip Mac resource forks. For source files that's fine — they're plain-text data-fork files — but it matters for anything carrying a resource fork. HFS+ media or a Mac-aware server preserves the forks. ([TinkerDifferent](https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/))

The resource-fork detail bites people, so it's worth understanding now. A classic Mac file has two parts: the data fork (ordinary file contents) and the resource fork (structured Mac-specific data — icons, the `'carb'` marker, and so on). Plain FAT-32 has no concept of a resource fork and drops it silently. The conventional way to move a Mac file with its fork intact through a non-Mac channel is to wrap it in a StuffIt `.sit` or BinHex `.hqx` archive, which is exactly how MacSurf's build pack ships. If you're transferring the StuffIt build pack, expand it *on the Mac* with StuffIt Expander and the forks come back intact.

If you also want to edit source on Linux and move it to the Mac, see [Cross-Developing from Linux](Cross-Developing-from-Linux) — it covers the line-ending detail (Mac source wants CR endings) and the syntax-check workflow.

## Route B: Emulation

No vintage hardware? You can do real work in an emulator — install CodeWarrior, build the project, launch the result, and confirm a page renders. That's a legitimate smoke test and it'll catch a lot. What it *won't* catch is hardware-specific behavior, and we'll come back to that honestly at the end.

Two important constraints up front. First, you need an emulator that emulates **PowerPC**, because MacSurf and OS 9 are PowerPC-only. Second — and this is the classic newcomer mistake — **Basilisk II will not work.** Basilisk II is a 68k Macintosh emulator; it tops out at Mac OS 8.1 and cannot run a PowerPC CPU at all. A CodeWarrior-built PowerPC Carbon binary, and OS 9 itself, won't run on it. Don't reach for it. ([cebix/macemu](https://github.com/cebix/macemu))

That leaves two realistic options: SheepShaver and QEMU.

### SheepShaver

SheepShaver is a PowerPC Macintosh emulator. On a PowerPC host it runs apps at native speed; on everything else (your x86 Linux box, for instance) it uses a built-in PowerPC interpreter. It runs on Linux, modern macOS, Windows, and more. ([sheepshaver.cebix.net](https://sheepshaver.cebix.net/))

The one ceiling you have to know: **SheepShaver runs Mac OS up to 9.0.4 and no higher.** This isn't a setting — SheepShaver doesn't emulate the PowerPC MMU (memory management unit), and OS 9.1 through 9.2.2 require it. ([Wikipedia: SheepShaver](https://en.wikipedia.org/wiki/SheepShaver)) MacSurf's stated target is 9.1+, and 9.0.4 is below that line. In practice the maintainer has run MacSurf builds under SheepShaver on 9.0.4 successfully for launch-and-render smoke tests, which makes it a perfectly good build-gate even though 9.0.4 isn't a *supported* MacSurf target. If you need to emulate a genuine 9.1/9.2.2 environment, that's QEMU's job (below).

Setting SheepShaver up, in broad strokes:

1. Get the SheepShaver build for your host OS from the project page or the macemu repository.
2. Supply two things SheepShaver needs to boot: a **PowerMac ROM image** and a **Mac OS install** (up to 9.0.4). SheepShaver cannot run without both. ([sheepshaver.cebix.net](https://sheepshaver.cebix.net/))
3. Configure a disk image, RAM, and screen size in SheepShaver's preferences, then boot the OS 9 installer and install to the disk image.
4. Once OS 9 is up, install CodeWarrior and CarbonLib inside the emulated machine exactly as you would on hardware (see the CodeWarrior section below).

On sourcing the ROM and OS media: a PowerMac ROM image is Apple copyrighted material, and OS 9 install media is Apple property too. Community archives exist because Apple stopped selling these decades ago, but their legal status hasn't changed. The neutral, low-risk path is to extract a ROM from a Power Mac you own and image your own OS 9 install CD. Established community sites generally won't host bare ROMs for this reason and point you to extracting your own.

**The networking caveat.** SheepShaver's Open Transport networking does not reach the live internet out of the box — it needs manual ethernet configuration, and even then it's finicky. For MacSurf that means an HTTPS fetch may hang until it times out rather than connecting. This is a test-environment limitation, not a MacSurf bug. SheepShaver is good for confirming the app launches, Carbon initializes, and rendering works; it is **not** a place to validate the fetcher or TLS behavior. That testing happens on hardware.

### QEMU (qemu-system-ppc)

QEMU emulates a complete PowerMac PowerPC system and, unlike SheepShaver, goes all the way to **Mac OS 9.2.2** — so it can stand up a real MacSurf-target environment. It offers two PowerMac machine types: `g3beige` (a Beige Power Mac G3) and `mac99` (a G4-class Mac). ([QEMU PowerMac docs](https://www.qemu.org/docs/master/system/ppc/powermac.html))

QEMU's nice trick is that it uses **OpenBIOS** — a free, open Open Firmware implementation — as the machine firmware, so **you don't need an Apple ROM at all.** People who go hunting for a "QEMU Mac ROM" are chasing something that doesn't exist for this path; the firmware comes from the `openbios-ppc` package (supplied via `-L pc-bios`). You still bring your own legally obtained OS 9 install media. ([QEMU PowerMac docs](https://www.qemu.org/docs/master/system/ppc/powermac.html))

A couple of QEMU gotchas worth knowing before you start:

- **RAM ceiling is hard.** OS 9 won't boot above 1024 MB under QEMU, and won't boot below 64 MB (128 MB is a sane minimum). Over-provisioning RAM is a common silent boot failure. ([devonhubner.org](https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/))
- **CPU flag for 9.0/9.1.** Installing 9.0 or 9.1 on QEMU needs `-cpu G3`; 9.2.x boots on the default `mac99` CPU. Forgetting the flag is a common boot failure. ([Computernewb wiki](https://computernewb.com/wiki/QEMU/Guests/Mac_OS_9))

A representative invocation to install 9.2.2 on the G4-class machine looks like:

```bash
qemu-system-ppc -L pc-bios -M mac99 -m 512 -boot d \
  -hda os9.img -cdrom MacOS9.iso
```

(Adjust the disk image, RAM, and boot order to taste; the cited install guide has a fuller walkthrough.)

> **TODO (verify):** MacSurf has been smoke-tested under SheepShaver on 9.0.4; the repo does not record a QEMU-on-9.2.2 MacSurf run. The QEMU facts above are about getting OS 9.2.2 itself running, not a confirmed MacSurf-under-QEMU result. If you go this route, treat it as new ground.

### Where to get CodeWarrior and CarbonLib

For both emulation and a hardware setup where the original CDs are long gone, the practical sources today are the retro-computing archives. Apple's own legacy download pages are mostly dead.

- **CodeWarrior Pro 8** is archived on the [Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x) (base disc image plus the 8.1/8.2/8.3 updaters) and the [Macintosh Repository](https://www.macintoshrepository.org/1351-codewarrior-pro-8-x). The Garden lists the base `CodeWarrior_8_Pro` disc image alongside `CW_8.1_Update_Installer`, `CW_8.2_Update_Installer`, and `CW_8.3_Update_Installer`.
- **CarbonLib 1.5 / 1.6 / 1.6.1** installers are on the [Macintosh Garden](https://macintoshgarden.org/apps/carbonlib) and [Macintosh Repository](https://www.macintoshrepository.org/17069-carbonlib). 1.6.1 (2003) is the highest version and the one to aim for.

## Installing CodeWarrior Pro 8

CodeWarrior is the IDE and PowerPC compiler that builds MacSurf. Once OS 9 is running — on hardware or in an emulator — the install is the same.

Install the base **8.0** release first, then apply the updaters. The updaters are *cumulative*, and the order matters because of how Metrowerks layered them:

1. Run the **CodeWarrior Pro 8.0** installer from the base disc. Install to the default location. When it asks which components to install, you need the **MacOS PowerPC C/C++ Compiler**, the **MacOS PowerPC Linker**, the **MSL C Libraries** (Metrowerks Standard Library — the C runtime), and the **Universal Headers** (Apple's Mac OS Universal Interfaces). You can skip the Java, Windows, and Palm OS tooling; MacSurf doesn't use any of it.
2. Apply the **8.1 updater** if you have it. The 8.2 updater accepts an 8.0 base directly, so 8.1 isn't strictly required as an intermediate — but applying it first does no harm, and it keeps you on Metrowerks' intended layering.
3. Apply the **8.2 updater** on top of that.
4. Apply the **8.3 updater.** This one is important: 8.3 is intended to layer *only* on top of 8.2. You cannot drop 8.3 onto a bare 8.0 install — 8.2 has to be present first. Run them in order.

After 8.3 you're at the final CodeWarrior Pro 8 release, which is the toolchain MacSurf is built and verified with. ([Macworld 8.2](https://www.macworld.com/article/155573/codewarrior-5.html), [Macworld 8.3](https://www.macworld.com/article/156765/codewarrior-6.html), [Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x))

> Community install lore worth a heads-up (reported, not from a primary Metrowerks doc): some Mac OS 9 installs of CodeWarrior need the `MetroNub` extension placed in the System Folder's Extensions folder, and the `Metrowerks` folder dragged into System Folder → Preferences. If the IDE won't launch cleanly after install, that's the first thing to check. ([Macintosh Repository](https://www.macintoshrepository.org/1351-codewarrior-pro-8-x))

### Installing CarbonLib

CarbonLib ships with every OS 9, so this is almost always an update to a newer version rather than a first install. Grab a CarbonLib 1.6+ installer (1.6.1 is the highest), run it, and let it place the updated `CarbonLib` extension in your Extensions folder. Restart. MacSurf's shipped binary needs CarbonLib **1.5+** to run; **1.6+** is recommended for building. ([Macintosh Garden: CarbonLib](https://macintoshgarden.org/apps/carbonlib))

## Verifying the toolchain

Before you sink time into a full MacSurf build, confirm the pieces are actually in place:

1. **CodeWarrior launches.** Open the CodeWarrior IDE once. It should come up and create its preferences in the System Folder. If it crashes or refuses to launch, revisit the MetroNub/Metrowerks-folder note above and confirm CarbonLib is installed.
2. **The Universal Headers are present.** These are Apple's interface headers — `Carbon.h`, `MacTypes.h`, and the rest of the Toolbox declarations — and the compiler can't build anything Mac-specific without them. Confirm they landed under your CodeWarrior install (the installer puts them under `MacOS Support`). If they're missing, re-run the 8.0 installer with the Universal Headers component checked.
3. **CarbonLib is in Extensions.** Open the Extensions folder in your System Folder and confirm `CarbonLib` is there at the version you installed. The MacSurf binary binds to it at runtime; without it, a Carbon app fails to launch.
4. **Build a trivial project (optional but reassuring).** If you want certainty that the compiler and linker work end to end before tackling MacSurf, create a tiny "hello" Carbon stationery project from CodeWarrior's new-project templates and build it. A clean build means your toolchain is sound and any later errors are about MacSurf's source, not your setup.

When all four check out, you're ready. Move on to [CodeWarrior Project Settings](CodeWarrior-Project-Settings) to understand the exact target configuration MacSurf needs, then [Building MacSurf](Building-MacSurf) to do the build.

## Honest limits: emulation is a smoke test, not the truth

Emulation earns its keep — you can develop, build, and confirm the app launches and renders without owning a single piece of vintage hardware. Use it freely for that.

But an emulator is more forgiving than a real machine, and that forgiveness hides exactly the bugs that bite hardest. SheepShaver's CarbonLib and Control Manager emulation tolerates call paths that a real G3 or G4 rejects outright, so a clean run in the emulator is a green light for "does it build and render," not for "is it correct on hardware." Hardware-specific crashes — input-device handling, scroll-bar tracking, low-level Toolbox edge cases — don't reliably reproduce in the emulator. Neither does real networking; SheepShaver's TCP doesn't reach the live internet without a fight, so the fetcher and macTLS get exercised on hardware, not in emulation. This isn't a SheepShaver bug — it's the maintainer's own experience that the emulator's CarbonLib and Control Manager are more permissive than a real G3/G4, which is why a green light in emulation only ever means "it builds and renders."

So the rule of thumb: **build and smoke-test wherever is convenient; trust only what a real G3 or G4 tells you.** If you're contributing a fix that touches the event loop, rendering on real pages, input, or networking, verify it on hardware before you consider it done. For more on what each environment can and can't tell you — and the debug tooling that bridges the gap — see [Diagnostics & Debugging](Diagnostics-and-Debugging).

## Sources

- CodeWarrior Pro 8: https://macintoshgarden.org/apps/codewarrior-pro-8x · https://www.macintoshrepository.org/1351-codewarrior-pro-8-x
- CodeWarrior 8.2 / 8.3 updaters: https://www.macworld.com/article/155573/codewarrior-5.html · https://www.macworld.com/article/156765/codewarrior-6.html
- CarbonLib: Apple *Carbon Porting Guide* https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf · https://macintoshgarden.org/apps/carbonlib · https://www.macintoshrepository.org/17069-carbonlib
- Real hardware (models, install, storage, networking, transfer): https://everymac.com/systems/apple/powermac_g3/faq/power-mac-g3-boot-mac-os-9-x-support.html · https://everymac.com/systems/apple/powermac_g4/faq/power-mac-g4-boot-macos-9-run-classic-applications.html · https://en.wikipedia.org/wiki/Mac_OS_9 · https://www.macintoshrepository.org/2605-mac-os-9-0-4-9-1-9-2-1-9-2-2-international-english-updaters · https://lowendmac.com/2013/low-end-macs-compleat-guide-to-mac-os-9/ · https://lowendmac.com/2010/sata-and-ssd-options-for-g3-and-g4-power-macs/ · https://lowendmac.com/2014/maximum-hard-drive-size/ · https://www.macworld.com/article/227653/how-to-connect-an-old-power-macintosh-g3-and-other-vintage-macs-to-the-internet.html · https://tinkerdifferent.com/threads/easy-way-to-transfer-files-to-and-from-imac-g3.2028/ · https://mac-classic.com/articles/remote-file-storage-transfers-and-backups/
- Emulation (SheepShaver, QEMU, why not Basilisk II): https://sheepshaver.cebix.net/ · https://en.wikipedia.org/wiki/SheepShaver · https://github.com/cebix/macemu · https://www.qemu.org/docs/master/system/ppc/powermac.html · https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/ · https://computernewb.com/wiki/QEMU/Guests/Mac_OS_9
