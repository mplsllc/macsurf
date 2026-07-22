# Emulating Mac OS 9 / PowerPC (for running PPC software and CodeWarrior)

Research notes for the MacSurf wiki. MacSurf targets PowerPC Mac OS 9, so any
emulator used to smoke-test builds or run CodeWarrior must emulate the **PowerPC**
architecture, not just 68k. The two realistic options are **SheepShaver** and
**QEMU system-ppc**. **Basilisk II is not an option** because it is 68k-only.
All facts below were verified by fetching the cited pages directly.

## SheepShaver (PowerPC emulator)

- SheepShaver emulates PowerPC Macintosh systems; on a PowerPC host it runs apps at native speed, and on non-PowerPC hosts it uses a built-in PowerPC emulator. Source: https://sheepshaver.cebix.net/
- It runs **Mac OS 7.5.2 through Mac OS 9.0.4**. Mac OS X as a guest is not supported. Source: https://sheepshaver.cebix.net/
- The 9.0.4 ceiling is real and architectural: SheepShaver does **not emulate the MMU (memory management unit)**, so Mac OS 9.1–9.2.2 are unsupported. Source: https://en.wikipedia.org/wiki/SheepShaver
- It **requires a copy of Mac OS plus a PowerMac ROM image** to run. Source: https://sheepshaver.cebix.net/
- SheepShaver was originally commercial software released in 1998, became open source in 2002, was conceived/programmed by Christian Bauer, and is now developed by Gwenolé Beauchesne. Source: https://en.wikipedia.org/wiki/SheepShaver
- It runs on Unix/X11 (Linux i386/x86_64/ppc, NetBSD, FreeBSD), Mac OS X (PowerPC and Intel), Windows NT/2000/XP, and BeOS R4/R5. Source: https://sheepshaver.cebix.net/
- SheepShaver and Basilisk II share one source repository (cebix/macemu). Source: https://github.com/cebix/macemu

## QEMU system-ppc (full PowerMac system emulation)

- The `qemu-system-ppc` binary emulates a complete PowerMac PowerPC system. QEMU provides two PowerMac machine types: **`g3beige`** (Heathrow-based, a Beige Power Mac G3) and **`mac99`** (a G4-class Mac). Source: https://www.qemu.org/docs/master/system/ppc/powermac.html
- QEMU uses **OpenBIOS** (a free GPLv2, IEEE 1275 / Open Firmware implementation) as the firmware for both g3beige and mac99 — so **no proprietary Apple Mac ROM is required**. Source: https://www.qemu.org/docs/master/system/ppc/powermac.html
- Emulated PowerMac peripherals include a UniNorth or Grackle PCI bridge, a VGA card with VESA/Bochs extensions, two PMAC IDE interfaces (HDD + CD-ROM), NE2000 PCI network adapters, NVRAM, and a VIA-CUDA with ADB keyboard and mouse. Source: https://www.qemu.org/docs/master/system/ppc/powermac.html
- A working install of **Mac OS 9.2.2** on QEMU uses `-M mac99` with OpenBIOS supplied via `-L pc-bios`; e.g. `qemu-system-ppc -vnc :1 -L pc-bios -boot d -M mac99 -m 1024 ...`. The author installs `openbios-ppc` rather than any Mac ROM. Source: https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/
- RAM constraints: Mac OS 9 will not boot with more than 1024 MB, and will not boot with less than 64 MB (128 MB is a reasonable minimum). Source: https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/
- Practical guides confirm both machine types work; for **Mac OS 9.0 or 9.1 you must add `-cpu G3`**, while 9.2.x boots on the default mac99 CPU. Source: https://computernewb.com/wiki/QEMU/Guests/Mac_OS_9

## Why Basilisk II is NOT suitable

- Basilisk II is an open-source **68k** Macintosh emulator. It emulates either a Mac Classic (System up to 7.5) or a Mac II-series machine (Mac OS 7.x, 8.0, 8.1). It does **not** emulate PowerPC at all. Source: https://github.com/cebix/macemu
- Because Mac OS newer than 8.1 and all PowerPC-only applications require a PowerPC CPU, a 68k emulator like Basilisk II cannot run them. SheepShaver targets PowerPC and Mac OS through 9.0.4; Basilisk II tops out at Mac OS 8.1. Source: https://github.com/cebix/macemu
- Net effect for MacSurf: a CodeWarrior-built PowerPC Carbon binary, and Mac OS 9 itself, **will not run under Basilisk II**. Use SheepShaver (≤9.0.4) or QEMU mac99 (up to 9.2.2).

## ROM and OS 9 media — legality / sourcing (neutral terms)

- SheepShaver needs an Apple PowerMac ROM image and a Mac OS install. Bare ROM files are Apple copyrighted material; distributing them is generally understood to conflict with Apple's license terms, so authoritative community sites (e.g. E-Maculation) do not host or directly link bare ROMs and instead point users to extract a ROM from hardware they own. (Stated in search excerpts; the E-Maculation pages return HTTP 403 to automated fetches, so this is reported as community guidance rather than a directly verified quote.)
- QEMU sidesteps the ROM-copyright question entirely by using OpenBIOS instead of an Apple ROM. You still supply your own legally obtained Mac OS 9 install media (CD image). Source: https://www.qemu.org/docs/master/system/ppc/powermac.html
- Mac OS 9 install media is also Apple property; the neutral, low-risk path is to image your own retail/CD install. Apple has not sold these for decades, which is why community archives exist, but their legal status is unchanged.

## Beginner gotchas / things that surprise people

- **"PowerPC emulator" ≠ "Mac emulator."** Picking Basilisk II for Mac OS 9 is the classic mistake — it cannot run any PPC OS or app. Confirm the tool emulates PowerPC first.
- **SheepShaver stops at 9.0.4**, not 9.2.2 — and the reason is the missing MMU emulation, not a configuration you can flag your way around. For 9.1/9.2.x you need QEMU. (Source: Wikipedia, above.)
- **QEMU needs no Apple ROM** but does need OpenBIOS (`-L pc-bios` / the `openbios-ppc` package) — people who go hunting for a "QEMU Mac ROM" are looking for something that does not exist for this path.
- **The 1024 MB RAM ceiling is hard** — Mac OS 9 refuses to boot above it under QEMU; over-provisioning RAM is a common silent failure.
- **9.0/9.1 vs 9.2 CPU flag** — forgetting `-cpu G3` for 9.0/9.1 on QEMU is a common boot failure.
- **Emulator ≠ real hardware for low-level behavior.** Per MacSurf's own experience, SheepShaver's CarbonLib/Control-Manager emulation is more forgiving than a real G3/G4; a clean run in the emulator is a smoke test, not proof of hardware behavior (especially for crashes, USB, and live networking).

## Sources

- https://sheepshaver.cebix.net/ (Official SheepShaver home page)
- https://en.wikipedia.org/wiki/SheepShaver (SheepShaver — Wikipedia)
- https://github.com/cebix/macemu (cebix/macemu — Basilisk II and SheepShaver source repo)
- https://www.qemu.org/docs/master/system/ppc/powermac.html (QEMU PowerMac boards documentation)
- https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/ (Install Mac OS 9.2.2 on a QEMU PowerPC VM)
- https://computernewb.com/wiki/QEMU/Guests/Mac_OS_9 (How to install Mac OS 9 in QEMU — Computernewb Wiki)
