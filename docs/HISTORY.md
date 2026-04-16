# MacSurf: The Story of JavaScript on Mac OS 9

## The First ES5 JavaScript Engine on Classic Macintosh

On April 15, 2026, a Power Macintosh running Mac OS 9 executed a JavaScript program and displayed the result in a web browser window. The title bar read "MacSurf - JS OK (1+1=2)." It was the first time a modern ECMAScript engine had ever run on Classic Mac OS hardware.

By the end of that same night, MacSurf was rendering ASCII Mandelbrot fractals computed entirely in JavaScript, sorting 5,000-element arrays via quicksort, evaluating the Ackermann function to depth 1,021, and parsing JSON — all inside a Carbon application on a machine from 1998.

No consumer browser has ever done this before. Classic IE 5.x shipped JScript 5, but it predates ES5 and ran on Mac OS X, not Classic. Classilla attempted Mozilla's SpiderMonkey on OS 9 but JavaScript was its primary source of crashes and instability. MacSurf is the first browser to ship a stable, fully functional ES5 engine on this platform.

---

## The Machine

**Power Macintosh G3 Minitower (Beige)**
- Processor: PowerPC 750 (G3)
- RAM: 192 MB
- OS: Mac OS 9.1
- Compiler: Metrowerks CodeWarrior 8 Pro (8.3 update)
- Display: 1024x768, millions of colors

This is the development machine. Every line of code was compiled on it. Every screenshot was captured from it. Every benchmark was measured on it. There is no emulator in this story.

---

## The Technology Stack

MacSurf exists because of several remarkable pieces of technology, most of them older than the machine itself:

### NetSurf (netsurf-browser.org)
The browser engine. An open-source web browser written in C with no external dependencies, designed for low-resource systems. NetSurf's clean architecture — with explicit support for cooperative multitasking and non-POSIX platforms via its frontend abstraction — made the port to Mac OS 9 possible. The RISC OS and AmigaOS frontends served as direct references. MacSurf links against all five NetSurf core libraries: libparserutils, libhubbub, libdom, libcss, and libwapcaplet — approximately 125,000 lines of C across 443 source files.

### Duktape 2.7.0 (duktape.org)
The JavaScript engine. An embeddable ES5/ES5.1 interpreter written in portable C89 by Sami Vaarala. Duktape's single-file amalgamation (`duktape.c`, 3.6 MB) compiled under CodeWarrior 8's strict C89 mode with zero source patches to the engine itself — only configuration changes in `duk_config.h`. The engine provides full ES5.1 compliance including regular expressions, JSON, closures, prototypes, exception handling, and the complete set of built-in objects. Its ~200 KB code footprint and configurable heap make it viable on machines with as little as 64 MB RAM.

### Metrowerks CodeWarrior 8 Pro
The compiler. The last professional C/C++ IDE for Classic Mac OS, running natively on the same machine it targets. CW8's C89 mode, while strict, proved capable of compiling a combined codebase of over 130,000 lines including the 3.6 MB Duktape amalgamation — the largest single translation unit ever compiled on this platform for this project. The 8.3 update's improved optimizer generates reasonable PowerPC code despite the C89 constraints.

### Carbon API (Apple)
The application framework. Carbon provides the window manager, event loop, controls, QuickDraw drawing, and Open Transport networking that MacSurf uses. Designed as a bridge between Classic Mac OS and OS X, Carbon gave MacSurf access to modern (for 2001) UI primitives while remaining compatible with the cooperative multitasking model of OS 9.

### Open Transport (Apple)
The networking layer. MacSurf uses plain (non-InContext) Open Transport calls for TCP/IP, matching the patterns established by cy384's SSHeven and the Retro68 OT TCP demo. Synchronous blocking calls with `OTUseSyncIdleEvents` and a yield-to-thread notifier keep the UI responsive during network operations.

### MacSurf Proxy (custom, Go)
The TLS termination layer. A single Go binary running on a VPS (Hetzner, Germany) that receives plain HTTP from the Mac, fetches the requested URL via HTTPS, and returns the response as plain HTTP. No configuration files, no dependencies. The proxy is what makes modern HTTPS websites accessible to a machine that has no TLS stack.

### Claude (Anthropic)
The development partner. The MacSurf codebase — frontend, library ports, build system integration, JavaScript engine wiring, and proxy — was developed in collaboration with Claude, Anthropic's AI assistant. Claude wrote the C89-compatible source code, diagnosed CodeWarrior compilation errors from error logs transmitted via reverse SSH tunnel from the Mac, and iteratively debugged runtime crashes from CW8's debugger output and screenshots of the physical machine. The development workflow was: write code on a Linux server, zip it, SCP it to the Mac via reverse tunnel, compile on the Mac, photograph or transcribe errors, send them back, iterate.

---

## The Timeline

### v0.1 — The Browser Shell (March-April 2026)
- Carbon window with toolbar, URL bar, scrollbar
- Open Transport HTTP fetcher via proxy
- HTML tag stripping + word-wrap text renderer
- Cooperative event loop with WaitNextEvent
- Working navigation: back, forward, reload, home
- FrogFind.com loading successfully on real hardware

### v0.2 — The NetSurf Core (April 15, 2026)
- All five NetSurf libraries linked (443 .c files, ~125K LOC)
- nsoption, nscolour, system colour subsystems initialized
- Full NetSurf js_thread API wired
- Browser boots through netsurf_init without crashing
- Real content fetching through the v0.1 OT path

### v0.2-moonshot — JavaScript (April 15-16, 2026)
- Duktape 2.7.0 integrated in a single session
- Hand-crafted duk_config.h for PPC big-endian + CW8 C89
- Zero patches to duktape.c itself
- `1+1=2` smoke test passed on first run
- First real JavaScript-bearing web page loaded from mac.mp.ls
- 12 stress tests executed: closures, prototypes, quicksort, regex, promises, Ackermann, hash tables, matrix multiply, BST, and ASCII Mandelbrot
- All tests pass on real hardware

---

## The Benchmarks

Measured on the Power Macintosh G3, Mac OS 9.1, 192 MB RAM.
Duktape 2.7.0, DUK_USE_NATIVE_CALL_RECLIMIT 128.

| Test | Result | Time |
|---|---|---|
| Linked list: 10,000 nodes + reversal | Correct | <1 ms |
| Regex: email extraction from prose | 4/4 found | <1 ms |
| Promise polyfill: 3-step chain | Correct (6) | <1 ms |
| BST: 15-node insert + in-order traversal | Correct | <1 ms |
| Matrix multiply: 30x30 | Correct | <1 ms |
| Event emitter: 1,000 events x 2 listeners | 2,001 fired | <1 ms |
| Quicksort: 5,000 random integers | Verified sorted | 1,000 ms |
| Hash table: 10,000 insert + lookup | 10,000/10,000 | 1,000 ms |
| String builder: 100,000 chars (array+join) | 100,000 length | 2,000 ms |
| 100K Math.sqrt + Math.sin loop | Completed | 3,000 ms |
| Ackermann(3,7) | 1,021 (correct) | 5,000-6,000 ms |
| Mandelbrot: 40x20 ASCII, 80 max iterations | Renders correctly | ~2,000 ms |

---

## The Screenshots

### First JavaScript output on Mac OS 9
![MacSurf displaying the first JS test page](screenshots/MacSurfer%20%201.jpg)

The moment: `Hello from Duktape on Mac OS 9!` followed by `Math.sqrt(2) = 1.4142135623730951`. Full IEEE 754 double-precision floating point, computed by an ES5 engine, displayed in a Carbon window via QuickDraw, on a Power Macintosh from 1998.

### Advanced ES5 features
![Closures, prototypes, array methods, regex](screenshots/MacSurfer%20%202.jpg)

Constructor functions with prototype chains (`rex instanceof Dog: true, rex instanceof Animal: true`), Array.prototype.map/filter/reduce, and regex capture groups — all executing correctly.

### Exception handling, JSON, date arithmetic
![Try/catch, JSON round-trip, Date](screenshots/MacSurfer%20%203.jpg)

Custom Error objects caught via try/catch, JSON.parse throwing SyntaxError on invalid input, JSON.stringify with nested arrays and mutation, and Date arithmetic using GetDateTime() converted from Mac epoch (1904) to Unix epoch (1970).

### Fibonacci, benchmarks, strict mode
![Recursion, performance, ES5 strict](screenshots/MacSurfer%20%204.jpg)

`fib(20) = 6765` via naive recursion. 100,000 sqrt+sin operations in 3,000 ms. ES5 strict mode, Array.isArray, Object.keys — all correct.

### Stress test v1: pushing the limits
![Stress test showing OOM at 50K hash table](screenshots/MacSurf5.jpg)

The first stress test run. Ackermann(3,7) = 1021 completed successfully despite the 128-frame native call recursion limit. The 50,000-entry hash table hit the Duktape heap ceiling — the first real constraint we found. Not CPU, not stack: memory.

### Stress test v2: all 12 pass
![All stress tests passing](screenshots/MacSurf6.jpg)

After reducing hash table to 10K entries and switching string concatenation from += to array+join (20x speedup), all 12 stress tests complete successfully.

### The Mandelbrot
![ASCII Mandelbrot fractal computed by Duktape](screenshots/MacSurf7.jpg)

An ASCII Mandelbrot set, 40x20 characters, 80 iterations per pixel, computed entirely in JavaScript by Duktape on a PowerPC G3 running Mac OS 9. The fractal that proved a 1998 machine can run a modern scripting language.

---

## What's Next

- **Image rendering**: BMP/GIF display via QuickDraw
- **Real HTML rendering**: port libdom's hubbub binding (dom_parser.c) to C89 for full DOM tree construction
- **CSS cascade**: wire libcss computed styles into the plotter pipeline
- **Proxy render-and-flatten**: headless Chromium on the proxy server executes JavaScript-heavy pages and sends pre-rendered HTML to MacSurf
- **Real-world site compatibility**: Hacker News, Wikipedia, DuckDuckGo

---

## Credits

**Patrick Britton** — Creator, hardware wrangler, build engineer, tester, and the person who believed a 1998 Power Mac could run JavaScript in 2026.

**Claude (Anthropic)** — Development partner. Wrote the C code, diagnosed CW8 errors from transcribed logs, debugged crashes from photographs of a CRT monitor, and kept shipping fixes until the Mandelbrot rendered.

**Sami Vaarala** — Creator of Duktape. The decision to write an ES5 engine in portable C89 with a single-file amalgamation is what made this entire project possible. Duktape compiled on CodeWarrior 8 with zero source patches.

**NetSurf contributors** — The NetSurf browser's clean C architecture, explicit POSIX-free design, and cooperative-multitasking frontend model gave MacSurf a real browser engine to build on.

**cy384** — Author of SSHeven and the Retro68 OT TCP demo. The Open Transport patterns MacSurf uses for networking are directly derived from cy384's verified-working implementations.

**Macintosh Garden (macintoshgarden.org)** — The indispensable archive of Classic Mac software. CodeWarrior 8, ResEdit, system software, extensions, utilities — if it ran on a classic Mac and you needed it, Macintosh Garden had it. This project would not have been possible without the preservation work of that community.

**Gary Hansen (RIP)** — The Power Macintosh G3 Minitower that MacSurf was built on was Gary's machine. Every screenshot, every benchmark, every late-night debugging session happened on hardware that outlived its original owner. Gary's G3 is still compiling code and rendering fractals in 2026. This project is dedicated to his memory.

**The Classic Mac community** — Everyone keeping these machines alive, sharing knowledge about CarbonLib, Universal Interfaces, and the art of writing software for an OS that Apple abandoned 24 years ago.

---

*MacSurf is open source. The code, the story, and the screenshots are all real. No emulators were used. No shortcuts were taken. Gary's Power Macintosh G3 from 1998 runs JavaScript in 2026.*
