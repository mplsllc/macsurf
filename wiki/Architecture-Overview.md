# Architecture Overview

This is the map. If you want to understand how MacSurf turns a URL you typed on a 1999 iMac into pixels on screen — or you're about to change a piece of it and need to know what you're touching — start here. We'll walk the whole system once, end to end, and link out to the deeper pages where each part earns its own chapter.

A quick orientation before the details: MacSurf is a fork of the **NetSurf** browser engine with a custom Mac OS 9 frontend. NetSurf is a small, portable, from-scratch browser engine written in C — not a WebKit or Gecko wrapper, but its own HTML parser, CSS engine, DOM, layout, and rendering, designed from the start to run well on slow hardware ([netsurf-browser.org/about](https://www.netsurf-browser.org/about/)). That last part is exactly why it's a sane choice for a Power Mac G3. NetSurf already knows how to separate its portable core from a platform-specific "frontend," and MacSurf is one more frontend — the `macos9` one — bolted onto a tree of ported NetSurf libraries, with a native TLS stack underneath for HTTPS.

## The big picture

Think of MacSurf as four layers stacked on top of each other:

1. **The Carbon frontend** (`browser/netsurf/frontends/macos9/`) — everything that touches the Mac: the window, the toolbar, the event loop, QuickDraw drawing, Open Transport networking, and the glue that connects NetSurf's expectations to Mac OS 9's reality.
2. **NetSurf core** (`browser/netsurf/`) — the portable browser engine: fetch dispatch, content caching, the HTML and CSS content handlers, the layout engine, and the abstract "plotter" interface that drawing flows through.
3. **The ported engine libraries** (`browser/lib*`) — NetSurf's reusable building blocks, each carried over to compile under CodeWarrior 8 in strict C89: `libparserutils`, `libwapcaplet`, `libhubbub`, `libdom`, `libcss`, and the `libduktape` JavaScript engine.
4. **macTLS** — a separate native TLS library that lets the Mac do HTTPS itself, directly to the origin server, over Open Transport. No intermediary.

Everything in layers 2 and 3 is more-or-less stock NetSurf, fought into shape for an ancient compiler. Layers 1 and 4 are the parts that are uniquely MacSurf, and they're where most of the interesting and hardware-specific work lives. The two pages that go deepest on the moving parts are [The Rendering Pipeline](The-Rendering-Pipeline) and [Networking & TLS](Networking-and-TLS); this page connects them.

## Why a fork of NetSurf at all

You could ask why MacSurf doesn't just write a browser from nothing. The honest answer is that a browser is enormous, and the hard parts — a spec-faithful HTML5 parser, a CSS cascade, a box-model layout engine — are years of work each. NetSurf has already done that work, in portable C, with the explicit goal of running on constrained machines. Equally important, its engine pieces are split into standalone libraries that ship under the MIT license (the browser application itself is GPLv2), which is what makes embedding them into a port like this legally and practically clean ([libcss README](https://github.com/netsurf-browser/libcss), [libdom README](https://github.com/netsurf-browser/libdom)). MacSurf's job is therefore not "build a web engine" but "teach an existing web engine to live on Mac OS 9" — which is plenty.

For the project's license specifically, see the repo `LICENSE`; this page isn't legal advice.

## The ported libraries

NetSurf's engine is deliberately modular, and the five core parsing/style libraries stack in a strict dependency order. MacSurf ports all of them, plus the JavaScript engine. Each one had to be made C89-clean for CodeWarrior 8 — no C99, no `//` comments, no designated initializers — which is a recurring theme you'll meet again on [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas).

| Library | Lives at | Job |
|---|---|---|
| `libparserutils` | `browser/libparserutils/` | Low-level parsing helpers: character encoding, input streams. The bottom of the stack. |
| `libwapcaplet` | `browser/libwapcaplet/` | String interning — stores small strings once and compares them by identity, with a built-in caseless path. Used everywhere a tag or property name needs fast equality. |
| `libhubbub` | `browser/libhubbub/` | The HTML5 parser. Turns a byte stream of HTML into parser tokens, faithfully following the HTML5 tree-construction rules. |
| `libdom` | `browser/libdom/` | A C implementation of the W3C DOM. The parser feeds this; it's the live tree the rest of the engine and JavaScript walk. |
| `libcss` | `browser/libcss/` | The CSS parser and selection/cascade engine. Parses stylesheets and computes the final style for each element. |
| `libduktape` | `browser/libduktape/` | Duktape 2.7.0, an embeddable ES5 JavaScript interpreter. |

The dependency chain matters because you can't build the upper layers without the lower ones: `libparserutils` and `libwapcaplet` sit at the bottom, `libhubbub` builds on them, `libcss` needs both base libraries, and `libdom` needs the base libraries plus `libhubbub` ([NetSurf libraries](https://www.netsurf-browser.org/projects/)). When you're setting up the CodeWarrior project, this is the order things have to compile in, and it's reflected in the access-path structure documented on [CodeWarrior Project Settings](CodeWarrior-Project-Settings).

`libcss` is by far the largest of these (hundreds of `.c` files) and the one most actively extended in MacSurf, because modern CSS is where the real-web-rendering battle is fought. [The Rendering Pipeline](The-Rendering-Pipeline) covers what's implemented versus parsed-but-not-yet-consumed.

## The Carbon frontend

The frontend is the Mac-native code that NetSurf core calls into. Mac OS 9 programs talk to the system through the **Macintosh Toolbox** — the set of system routines for windows, menus, events, and drawing ([Macintosh Toolbox](https://wiki.retrotechcollection.com/Macintosh_Toolbox)). MacSurf uses the Toolbox through **Carbon**, a C API designed so one source base can run on both Classic Mac OS (via the CarbonLib shared library) and early Mac OS X ([Carbon API](https://en.wikipedia.org/wiki/Carbon_(API))). That's why one MacSurf binary runs on OS 9 today and would, in principle, run on an early OS X — the Carbon contract is the same.

The frontend lives in `browser/netsurf/frontends/macos9/`. The files split cleanly by responsibility, which is the convention to keep: Toolbox calls stay isolated in their own files rather than smeared across the codebase.

| File | Responsibility |
|---|---|
| `main.c` | Entry point. Carbon/Open Transport startup, the `WaitNextEvent` event loop, event dispatch, and `netsurf_init`. |
| `window.c` | The Carbon window, toolbar (back/forward/reload/home), URL field, scrollbar; navigation and history orchestration. |
| `plotters.c` | The QuickDraw implementations of NetSurf's plotter callbacks — text, rectangles, clips, bitmaps. Where layout becomes pixels. |
| `macos9_font.c` | Font selection and text measurement. |
| `macos9_image.c`, `lodepng.c` | Image decode (PNG via lodepng, plus GIF/BMP/TIFF/JPEG) into NetSurf bitmaps. |
| `macos9_http_fetcher.c`, `macos9_https_fetcher.c`, `macos9_fetch.c` | The HTTP and HTTPS fetchers over Open Transport. |
| `schedule.c` | NetSurf's cooperative scheduler — timers and deferred work that fit the no-threads model. |
| `macsurf_dom_dispatch.c`, `macos9_js_click.c` | The bridge that wires Duktape and the DOM into the running page. |
| `macsurf_debug_log.c` | The file-backed diagnostic log — the primary post-crash forensics channel. See [Diagnostics & Debugging](Diagnostics-and-Debugging). |

A Carbon app on OS 9 has a few non-negotiable startup rules baked into this frontend, and they're worth knowing because getting them wrong produces crashes that look like something else entirely. The binary **must** carry a `'carb'` resource — a marker in its resource fork that tells the Code Fragment Manager "this is a Carbon fragment." Without it, CarbonLib never loads as a dependency and the first Carbon networking call crashes inside the system's own libraries. The app calls `RegisterAppearanceClient()` at startup so its controls render with the platinum theme, and it deliberately *skips* the old `InitGraf`/`InitFonts`/`InitWindows` initialization calls, because under Carbon, CarbonLib handles those itself. These requirements, the resource fork, and the `'carb'` marker are detailed on [CodeWarrior Project Settings](CodeWarrior-Project-Settings) and in the resource-fork discussion on [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project).

## The cooperative event loop

Here's the constraint that shapes nearly every design decision in MacSurf: **Mac OS 9 is cooperatively multitasked.** There is no preemption. Each program must voluntarily hand the CPU back; a long computation that never yields freezes the entire machine, not just the one app ([Apple multitasking concepts](https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html)). So MacSurf has no threads anywhere. The whole browser runs on a single `WaitNextEvent` loop in `main.c` — fetch a mouse click or keystroke or null event, dispatch it, draw, repeat. `WaitNextEvent` is the System 7+ call that fetches the next event and, crucially, yields the processor to the rest of the system while it waits ([mac toolbox followup](https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html)).

The obvious problem: a network fetch can block for seconds. If MacSurf sat blocked inside a socket read, the cursor would stop spinning and the machine would appear hung. The answer is cooperative yielding from inside the network calls themselves — Open Transport fires a periodic "I'm still waiting" event during a blocking call, and MacSurf's handler responds by yielding to the scheduler so the UI keeps breathing. [Networking & TLS](Networking-and-TLS) explains this yield mechanism in detail; the thing to carry forward here is that *everything* — fetch, parse, layout, draw, even JavaScript — is interleaved on one thread, and any code that doesn't yield politely is a bug waiting to freeze a real Mac.

## How a page flows from URL to pixels

Now the main event. You type a URL into the toolbar and press Return. Here's the journey, with each stage naming where it happens.

**1. Navigation kicks off.** `window.c` takes the text from the URL field, normalizes it (prepending `http://` if you left off a scheme), and hands it to NetSurf core's `browser_window_navigate`. The core decides it needs to fetch this URL and dispatches the request to the registered fetcher for the scheme.

**2. The fetch.** For `http:` the request goes to the Open Transport HTTP fetcher (`macos9_http_fetcher.c`); for `https:` it goes to the HTTPS fetcher (`macos9_https_fetcher.c`), which is where **macTLS** comes in. The fetcher opens an Open Transport TCP connection — Apple's classic TCP/IP stack, which dates back to System 7.5.2 in 1995 and is the standard networking stack on OS 8/9 ([Open Transport](https://en.wikipedia.org/wiki/Open_Transport)) — sends an HTTP/1.1 request (with chunked-encoding, keep-alive, and redirect handling), and for HTTPS does a full TLS 1.3 handshake on the Mac before any HTTP bytes move. Response bytes flow back to NetSurf core as a stream of fetch messages (`FETCH_HEADER`, `FETCH_DATA`, `FETCH_FINISHED`). Throughout, the fetcher yields cooperatively so the UI stays alive. There is **no proxy** in this path — the Mac talks straight to the origin server. The whole networking and TLS story is on [Networking & TLS](Networking-and-TLS).

**3. The parse.** NetSurf routes the response through its content system to the HTML content handler, which feeds the bytes to **libhubbub** (the HTML5 parser). Hubbub builds a **libdom** tree — the live DOM that everything downstream reads and that JavaScript mutates.

**4. The cascade.** As the document parses, stylesheets are fetched (more trips through stage 2) and parsed by **libcss**. For each element, libcss selects the matching rules and computes the final, resolved style — including resolving CSS custom properties and `var()` references natively, on the Mac, at cascade time. This is one of MacSurf's headline capabilities: real CSS3 custom properties on a G3.

**5. The layout.** NetSurf's layout engine walks the DOM with the computed styles and builds a box tree — figuring out where every block, inline run, flex item, and grid cell lands, and how tall the page is. Around 150 CSS properties are actually consumed here; the gap between "parsed by libcss" and "acted on by layout" is the frontier that [The Rendering Pipeline](The-Rendering-Pipeline) tracks property by property.

**6. The plot.** When a region needs drawing, NetSurf walks the laid-out box tree and calls its abstract **plotter** interface — `plot_text`, `plot_rectangle`, `plot_clip`, `plot_bitmap`, and friends. MacSurf's `plotters.c` implements those callbacks in **QuickDraw**, OS 9's native 2D graphics API: text via the font layer, fills via `PaintRect`, images via `CopyBits`/`CopyMask`. Drawing goes through an offscreen GWorld buffer that's then blitted to the window, which keeps redraws flicker-free and is the seam where most of the hard-won pixel-accuracy fixes live.

In one line: **fetch → parse (libhubbub → libdom) → cascade (libcss) → layout → plot (QuickDraw)**, all interleaved on the single cooperative event loop, with macTLS handling HTTPS underneath the fetch.

```
You type a URL
      │
      ▼
window.c ──► browser_window_navigate (NetSurf core)
      │
      ▼
HTTP / HTTPS fetcher (Open Transport ──► macTLS for HTTPS)   ← yields cooperatively
      │   FETCH_HEADER / FETCH_DATA / FETCH_FINISHED
      ▼
HTML content handler ──► libhubbub (parse) ──► libdom (DOM tree)
      │
      ▼
libcss (cascade, native var() resolution)
      │
      ▼
NetSurf layout engine (box tree, ~150 CSS properties consumed)
      │
      ▼
plotters.c (QuickDraw: text, rects, clips, bitmaps via an offscreen GWorld)
      │
      ▼
Pixels on the window
```

## Where JavaScript fits

JavaScript runs as part of the same flow, on the same single thread. When the parser meets a `<script>`, the HTML content handler hands it to **Duktape** (`libduktape`), the embedded ES5 interpreter linked into the base build. Duktape doesn't know about the DOM by itself, so MacSurf wires a browser runtime around it on-device: DOM element and document wrappers, timers (`setTimeout`/`setInterval`/`requestAnimationFrame`), `window.location` and `window.history`, `addEventListener`, `fetch`, `localStorage`, and a good deal more. Script-driven DOM changes flow back into the same libdom tree, which can trigger a recascade and reformat — the rendering pipeline runs again over the mutated tree.

Two things to underline. First, there is no offload: whatever JavaScript runs, runs on the Mac. Second, Duktape is treated as a capable peer to the rest of the engine, not a toy — gaps in the browser runtime get filled in-house rather than waved away. The full story, including which APIs are wired and where the frontier is, lives on [The JavaScript Engine](The-JavaScript-Engine).

## macTLS: how the Mac speaks HTTPS

The modern web is HTTPS, and a Power Mac G3 has no business doing TLS 1.3 — which is exactly why MacSurf does it. **macTLS** is a native TLS stack for Classic Mac OS, maintained in a sibling repository ([github.com/mplsllc/macTLS](https://github.com/mplsllc/macTLS)) and linked into the browser binary. It uses BearSSL for the cryptographic primitives under a hand-written TLS 1.3 handshake (per RFC 8446: X25519 and multi-curve ECDHE key exchange, ChaCha20-Poly1305 and AES-128-GCM ciphers), with transparent fallback to TLS 1.2, and the full Mozilla CA bundle — 121 trust anchors — baked into the binary so certificate validation works with no network round-trips for trust roots.

TLS needs good randomness, and an idle 1999 Mac is a poor source of entropy. macTLS solves that with **macEntropy**, a SHA-256 accumulator feeding an HMAC-DRBG, seeded from Open Transport packet jitter, event-loop input timing, a high-resolution clock, and a seed file persisted across launches. As far as anyone can tell, this is the **first native TLS 1.3 on Classic Mac OS** — running on real G3 hardware, no proxy, no screenshot trick, no remote terminal. The Mac genuinely opens the encrypted connection itself. The full handshake, the CA bundle, and the entropy design are documented on [Networking & TLS](Networking-and-TLS).

## Where to go next

This page is the hub; the spokes go deeper:

- [The Rendering Pipeline](The-Rendering-Pipeline) — libcss, native `var()`, what's implemented versus parsed-but-dropped, and the QuickDraw plotters.
- [The JavaScript Engine](The-JavaScript-Engine) — Duktape, `duk_config.h` tuning, the on-device DOM/BOM runtime, what works and what's still a frontier.
- [Networking & TLS](Networking-and-TLS) — Open Transport, the cooperative-yield fetch model, HTTP/1.1 details, and the macTLS / macEntropy story.
- [Building MacSurf](Building-MacSurf) — the master build walkthrough, from the StuffIt build pack or the source tree.
- [CodeWarrior Project Settings](CodeWarrior-Project-Settings) — exact target settings, access-path structure, the prefix file, partition size, and the resource file.
- [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) — the landmine list that explains why so much of this code looks the way it does.
- [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project) — the reusable lessons, if MacSurf inspired you to build your own thing.

> **TODO (verify):** The frontend file list above reflects the files present in `browser/netsurf/frontends/macos9/` and their roles as inferred from names and the project docs. The authoritative file-by-file set that's actually in the CodeWarrior project is on [CodeWarrior Project Settings](CodeWarrior-Project-Settings); if any role here disagrees with that page, that page wins.

## Sources

- NetSurf — what it is, its libraries, the GPL/MIT split: <https://www.netsurf-browser.org/about/>, <https://www.netsurf-browser.org/projects/>, <https://github.com/netsurf-browser/libcss>, <https://github.com/netsurf-browser/libdom>
- Carbon API and the Macintosh Toolbox: <https://en.wikipedia.org/wiki/Carbon_(API)>, <https://wiki.retrotechcollection.com/Macintosh_Toolbox>
- Cooperative multitasking and `WaitNextEvent`: <https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html>, <https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html>
- Open Transport: <https://en.wikipedia.org/wiki/Open_Transport>
- macTLS: <https://github.com/mplsllc/macTLS>
