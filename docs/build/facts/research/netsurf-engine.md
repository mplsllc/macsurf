# NetSurf Web Browser Engine

Background notes for the MacSurf wiki. NetSurf is the engine MacSurf is
built on, so this records what NetSurf actually is, how it is structured,
and the licensing that makes the MacSurf port possible. Facts below were
fetched and verified against primary sources (the official project site
and the per-library GitHub READMEs); a few items only verifiable on
Wikipedia are flagged as such.

## What NetSurf is

- NetSurf is a free, open-source web browser with its own layout and
  rendering engine "entirely written from scratch" (i.e. it is not a
  WebKit/Gecko/Blink shell). Source: https://www.netsurf-browser.org/about/
- It is written in C. The core browser is released under the GNU General
  Public Licence version 2. Source: https://www.netsurf-browser.org/about/
- Per Wikipedia, initial release was 19 May 2007; the language is ANSI C;
  it implements "most of the HTML 4 and CSS 2.1 specifications"; the most
  recent stable release noted is 3.11 (28 December 2023). These
  version/date specifics are from Wikipedia, not the project site, so
  treat the exact release date as secondary. Source: https://en.wikipedia.org/wiki/NetSurf
- Stated design goal is to be "lightweight and portable," and it performs
  well on resource-constrained hardware. This is the key reason it is a
  sensible base for a Mac OS 9 port. Source: https://www.netsurf-browser.org/about/

## Core libraries (the engine's modular pieces)

NetSurf deliberately splits its engine into standalone libraries that are
released separately so other software can reuse them. Each of the five
core parsing/style libraries below is released under the **MIT license**
(verified per-repo), even though the NetSurf application itself is GPL-2.0.

- **LibParserUtils** — "Parser building library" / parser-building utility
  functions; the lowest-level dependency. Source: https://www.netsurf-browser.org/projects/
- **LibWapcaplet** — a "string internment library": a reference-counted
  string interning system for storing small strings and doing rapid
  equality comparison, including an automatic caseless (lowercased)
  comparison path. MIT licensed. Source: https://github.com/netsurf-browser/libwapcaplet
- **Hubbub** — "HTML5 compliant parsing library" (the HTML parser).
  Source: https://www.netsurf-browser.org/projects/
- **LibCSS** — "a CSS parser and selection engine. It aims to parse the
  forward compatible CSS grammar." Requires LibParserUtils and
  LibWapcaplet. MIT licensed. Source: https://github.com/netsurf-browser/libcss
- **LibDOM** — "an implementation of the W3C DOM API in C." Requires
  LibParserUtils, LibWapcaplet, and LibHubbub. MIT licensed.
  Source: https://github.com/netsurf-browser/libdom

The dependency chain therefore stacks: LibParserUtils + LibWapcaplet at
the bottom, Hubbub on top of those, LibCSS on those, and LibDOM on top of
all of Hubbub/LibCSS's prerequisites. (Verified from the libcss and libdom
READMEs above.)

Other NetSurf sub-projects (not part of MacSurf's core five but part of the
larger engine ecosystem): Libsvgtiny (SVG Tiny), LibNSFB (framebuffer
abstraction), Libnsbmp (BMP/ICO decode), Libnsgif (GIF decode), plus
RISC-OS-specific helpers (LibROSprite, RUfl, TTF2f, Tinct, Libpencil).
Source: https://www.netsurf-browser.org/projects/

## Frontend / platform model

- NetSurf separates a portable core from per-platform "frontends." Known
  frontends include amiga, atari, beos, framebuffer, gtk, monkey, riscos,
  and windows. The GTK frontend was started in June 2004 and runs on
  Unix-like systems. Source: https://en.wikipedia.org/wiki/NetSurf
- Officially supported/targeted platforms include RISC OS (4+), AmigaOS 4,
  BeOS/Zeta/Haiku, Atari TOS, Windows, and Unix-like systems (Linux,
  FreeBSD, NetBSD, Solaris). Source: https://www.netsurf-browser.org/about/
- JavaScript support uses the Duktape engine (added in preview form around
  2012, later improved). The exact timeline is Wikipedia-sourced; the
  Duktape dependency itself is stated on the project's about page.
  Sources: https://www.netsurf-browser.org/about/ , https://en.wikipedia.org/wiki/NetSurf

## Beginner gotchas / things that surprise people

- **It is not based on WebKit.** People assume every small browser wraps an
  existing engine; NetSurf wrote its own layout/render engine in C.
- **Two different licenses in one project.** The browser app is GPL-2.0,
  but the five reusable engine libraries (libparserutils, libwapcaplet,
  hubbub, libcss, libdom) are MIT. That MIT licensing is what lets them be
  embedded in other-licensed projects.
- **"NetSurf" the app vs. the libraries.** The libraries ship and version
  independently of the browser; you can use libcss/libdom without NetSurf.
- **Standards coverage is dated by design.** Core CSS support targets CSS
  2.1 / HTML 4 era features (Wikipedia), so modern CSS3/JS-heavy sites are
  partial — expected for a lightweight engine.
- **Version/date specifics are secondary-sourced.** The 3.11 / 2023-12-28
  release and the 2007 initial release come from Wikipedia, not the
  project's own page; verify against netsurf-browser.org/downloads before
  quoting as authoritative.

## Sources

- https://www.netsurf-browser.org/about/
- https://www.netsurf-browser.org/projects/
- https://github.com/netsurf-browser/libwapcaplet
- https://github.com/netsurf-browser/libcss
- https://github.com/netsurf-browser/libdom
- https://en.wikipedia.org/wiki/NetSurf
