# MacSurf Documentation

Documentation for [MacSurf](../README.md), a NetSurf-based browser for Mac OS 9 PowerPC with native HTTPS.

## Start here

- [architecture.md](architecture.md), Full platform architecture: rendering modes, proxy services, template system, milestone plan.
- [status.md](status.md), Current project status, build state, what works today.
- [masterplan.md](masterplan.md), Maintained development roadmap.

## Build & deploy

- [build/automated-ship.md](build/automated-ship.md), **The automated pipeline: one command from Linux edit to running app and a `.sit` on macfiles.** Start here for the day-to-day loop.
- [build/hardware-bisect.md](build/hardware-bisect.md), **Finding a first-bad commit on real hardware.** Reverting a tree the Mac will actually build, what to do about files added since the baseline, and what dominates the cost per point.
- [codewarrior-setup.md](codewarrior-setup.md), Install CodeWarrior 8 and build on a real Power Mac.
- [cross-dev-from-linux.md](cross-dev-from-linux.md), Cross-compile workflow from Linux using Retro68.
- [deploying-proxy.md](deploying-proxy.md), Deploy the Go TLS-stripping proxy.
- [usb-overdrive.md](usb-overdrive.md), USB Overdrive configuration notes for Mac OS 9 dev hardware.

## CSS engine

- [css-support.md](css-support.md), User-facing property support summary.

## Research

Deep-dive engineering research and historical state snapshots live in [research/](research/). These are point-in-time investigations preserved for context, not always current.
