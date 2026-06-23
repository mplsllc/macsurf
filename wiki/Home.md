# MacSurf Wiki

**The modern web, on a 25-year-old Mac.**

MacSurf is a web browser for Classic Mac OS 9 on PowerPC. It's a fork of the [NetSurf](https://www.netsurf-browser.org/) engine wrapped in a hand-written Carbon frontend, and on a G3 iMac it renders a surprising amount of the real web: CSS3 layout, ES5 JavaScript with a real DOM, PNGs with alpha, and — as of v1.3 — **native TLS 1.3**, the first on Classic Mac OS that anyone's aware of. No proxy, no stripped-down "mobile" web. The actual internet, fetched and drawn by a machine from 1999.

This wiki is the manual: how MacSurf works, how to build it, and how to build your own Classic Mac software using the same hard-won techniques.

## A note on support, up front

MacSurf is a one-person, nights-and-weekends project. This wiki *is* the support. It's written so you can get unstuck on your own, because I can't offer one-on-one build help for a 30-year-old toolchain and still keep the browser moving forward. That's not coldness — it's the only way a solo project like this survives. If something here is wrong, unclear, or out of date, the most useful thing you can do is open an [issue](https://github.com/mplsllc/macsurf/issues) or a documentation PR. Corrections are genuinely welcome and they help the next person.

One thing worth knowing before you start: **MacSurf is not a clone-and-build project.** It's a Carbon application compiled with Metrowerks CodeWarrior 8 *on* Mac OS 9 (real hardware or an emulator). The supported, turnkey way to build it is the StuffIt build pack, opened in CodeWarrior — a bare `git clone` won't compile by itself, because the project's Mac-side wiring lives in the pack, not the tree. [Building MacSurf](Building-MacSurf) explains the whole path.

## Where to go from here

**Just want to understand it?** Start with the [Architecture Overview](Architecture-Overview) — the map of how a typed URL becomes pixels on screen — then dig into the [Rendering Pipeline](The-Rendering-Pipeline), the [JavaScript Engine](The-JavaScript-Engine), or [Networking & TLS](Networking-and-TLS).

**Want to build it?** Read [Building MacSurf](Building-MacSurf) first; it points you at [Setting Up the Build Environment](Setting-Up-the-Build-Environment) (CodeWarrior, CarbonLib, and Mac OS 9 on real hardware or in emulation) and the exact [CodeWarrior Project Settings](CodeWarrior-Project-Settings). If you edit on a modern machine, [Cross-Developing from Linux](Cross-Developing-from-Linux) covers the round-trip.

**Want to contribute?** [Contributing & Expanding](Contributing-and-Expanding) walks through adding a feature without breaking the cascade, the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page is the landmine map, and [Diagnostics & Debugging](Diagnostics-and-Debugging) shows how to see what the Mac is actually doing when something goes wrong.

**Working on your own old-Mac project?** [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project) distills everything that generalizes beyond MacSurf, and [Resources & Prior Art](Resources-and-Prior-Art) is the jumping-off point for the wider community and reference material.

## The state of things

MacSurf is young but real. The current release is **v1.4 "Open House"**, hardware-verified on a G3 iMac running Mac OS 9.2.2. It does native HTTPS over [Open Transport](Networking-and-TLS) with a hand-written TLS 1.3 stack (macTLS), runs JavaScript on-device with no offload of any kind, and consumes around 150 CSS properties in layout. It is also, honestly, an early-ish browser by a single person — heavy single-page-app frameworks are still a frontier, and the long tail of modern features is tracked openly in the [issue tracker](https://github.com/mplsllc/macsurf/issues). It won't replace your 2026 desktop browser. It will put your old Mac back on the web, and that's the point.
