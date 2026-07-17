# The JavaScript Engine

This page explains how MacSurf runs JavaScript on a Power Mac G3 — the engine it uses, the browser runtime built on top of it, how that runtime reaches the page's DOM, and where the hard edges still are. It's for anyone curious how a 1999 Mac executes modern-ish web scripts, and for anyone who wants to extend the runtime.

One thing to settle up front, because it shapes everything else: **whatever runs, runs on the Mac.** There is no remote helper, no server doing the hard parts and shipping back a flat page. MacSurf fetches a page directly (over native TLS — see [Networking & TLS](Networking-and-TLS)), parses it, and executes its scripts in-process on the PowerPC. When a feature is missing, the answer is to build it into the browser, not to hand the work off somewhere else. That principle is why the runtime surface below kept growing instead of plateauing.

## Duktape: a real ES5 engine in 64 MB

MacSurf embeds [Duktape](https://duktape.org/) 2.7.0, a small, portable JavaScript engine written in C. Duktape implements ECMAScript 5/5.1 (with a few semantics borrowed from later editions), and it's built to be dropped into a C host and driven through a compact stack-based API — close in spirit to embedding Lua. That portability is the whole reason it's a fit here: Duktape's own documentation claims it can run in 160 kB of flash and 64 kB of RAM, and it lists genuinely retro targets like AmigaOS and RISC OS among its known-working platforms ([Duktape portability wiki](https://wiki.duktape.org/portability)). A 233 MHz G3 with 64 MB of RAM is luxurious by those standards.

It's linked into the **base build** — not an optional add-on, not gated behind a faster-machine tier. The same engine runs on the 64 MB G3 floor that runs everywhere else. Earlier project notes described JavaScript as "small inline scripts only, real JS lives elsewhere." That's no longer how MacSurf works, and that framing is retired. ES5 is the language MacSurf gives you, and a substantial browser runtime sits on top of it.

Embedding Duktape is famously low-ceremony: you add three files to the build — `duktape.c`, `duktape.h`, and `duk_config.h` — and there's no separate library to link. In MacSurf those live in `browser/libduktape/`. The single `duktape.c` is a large amalgamated source file; that's by design, not a packaging accident.

What ES5 buys you, concretely, is more than a calculator. The engine handles closures, prototypes, the regular-expression engine, `JSON`, recursion deep enough to chew through `ackermann(3,7)` in about five or six seconds on a 233 MHz G3, and it'll render a Mandelbrot set in pure JavaScript if you ask it to. Promises aren't native to ES5, so MacSurf provides a polyfill rather than relying on Duktape's optional `Promise` built-in. The honest boundary is the language edition: ES6 syntax — `let`, `const`, arrow functions, template literals, classes — is not part of ES5, so source written for a 2026 browser will often fail to *parse* before it ever runs. That's a Duktape characteristic, not a MacSurf bug, and it's the single most common surprise for someone testing a modern site.

## Tuning `duk_config.h` for Mac OS 9 PowerPC

Duktape detects platform and compiler features automatically, but that detection leans on C99, and CodeWarrior 8 compiles in strict C89 (see [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas)). Without reliable auto-detection, you pin the platform's facts by hand in `duk_config.h` — Duktape designs the file precisely so it *can* be hand-edited as a last resort for exotic targets. Mac OS 9 on PowerPC with CodeWarrior 8 is about as exotic as it gets, so MacSurf's config carries an explicit override block, keyed on `__MACOS9__` / `__MWERKS__`, that runs *after* the stock defaults so each value wins unconditionally (`#undef` then `#define`).

The settings that matter:

| Setting | Value | Why |
|---|---|---|
| `DUK_USE_BYTEORDER` | `3` | PowerPC is big-endian. Stock detection can't reliably infer this without C99, so it's pinned. |
| `DUK_USE_PACKED_TVAL` | defined | Pointers are 32-bit, so Duktape's tagged value packs into 8 bytes — smaller and faster. |
| `DUK_USE_ALIGN_BY` | `8` | PPC needs 4-byte alignment minimum; 8 is safe for doubles. |
| `DUK_USE_NATIVE_CALL_RECLIMIT` | `128` | OS 9 cooperative threads have a limited stack. A conservative C-call recursion cap keeps a runaway script from blowing it. |
| `DUK_USE_ECMASCRIPT_CALL_RECLIMIT` | `256` | Same reasoning for the JS call depth. |
| `DUK_INLINE` / `DUK_ALWAYS_INLINE` / `DUK_NOINLINE` | empty | CodeWarrior 8 has no `inline` keyword and no `__attribute__`, so Duktape's inline hints become no-ops; the optimizer still decides where to inline. |
| `DUK_USE_PTHREAD_API` | undefined | There are no preemptive threads on OS 9. |
| `DUK_USE_FILE_IO`, `DUK_USE_DEBUG` | undefined | Dropped to avoid pulling in libc surface MacSurf doesn't have. |

There's one more piece of C89 plumbing at the *top* of the file: CodeWarrior 8 has no C99 `<stdint.h>` with the `uint_least*_t` / `uint_fast*_t` / `intmax_t` family that Duktape's type-detection block typedefs around. So MacSurf supplies those as plain typedefs *before* stock detection runs, which lets Duktape's own `typedef uint_least8_t duk_uint_least8_t;` lines resolve cleanly instead of erroring out.

The recursion limits deserve a second look, because they're the one place where MacSurf is deliberately stingier than the engine's defaults. OS 9 is cooperatively multitasked — there's no preemption, and the per-thread stack is finite and shared with everything else the browser is doing on that tick. A JavaScript stack overflow on a desktop is an exception; on OS 9 a blown C stack is a crash that can take the OS down with it. Capping recursion turns "runaway script" into a catchable error rather than a dead machine. The same instinct shows up in the fatal handler below.

## How a script gets from `<script>` to running

MacSurf plugs Duktape into NetSurf's existing script-dispatch machinery, so scripts flow through the normal page pipeline (see [The Rendering Pipeline](The-Rendering-Pipeline)). NetSurf calls `js_newheap` when a document needs a JavaScript context; MacSurf creates a Duktape heap with `duk_create_heap_default()`, then immediately installs the browser globals and the DOM bindings before any page script runs. When the page is torn down, `js_destroyheap` calls `duk_destroy_heap`, which frees every JS object at once and invalidates the context pointer. Page scripts themselves arrive through `js_exec`.

Two design choices in there are worth understanding, because they're what keeps a bad script from being fatal.

First, **everything goes through *protected* evaluation.** Duktape's plain `duk_eval_string` throws a C-level error via `longjmp` that, if nothing catches it, reaches the fatal handler. The protected variants (`duk_peval_string`, `duk_peval_string_noresult`) return an error code instead. MacSurf's internal eval helper uses the protected, no-result form, checks the return code, logs the error string on failure, and pops it — so a syntax error or a thrown exception in one script becomes a log line, not a crash, and the rest of the page keeps loading.

Second, **the fatal handler refuses to die.** Duktape's contract says a fatal handler must not return normally to the engine — the engine is telling you the heap is unusable. The obvious implementation, `exit()`, is exactly wrong on OS 9, where killing the app over one broken script can take the operating system with it. MacSurf's handler instead `longjmp`s to a recovery buffer that the eval wrapper arms before it calls in, so control unwinds cleanly back to the browser and the bad heap gets torn down. If no recovery buffer happens to be armed, it parks in a tight loop rather than returning, and the parent thread reclaims the context on its next pump. The whole posture is: a broken script is a normal event on the real web, and the browser has to survive it.

## The browser runtime — the "JavaScript marathon"

A language engine isn't a browser. What makes scripts on real pages do anything useful is the *runtime* — the `window`, `document`, DOM, timers, and the dozens of Web APIs that page authors assume are present. Building that surface on-device, in C89, against an ES5 engine, was the work of a sustained push across fix rounds fixes319–352 that the project calls the JavaScript marathon. It closed roughly two dozen tracked issues and landed in **MacSurf v1.4 "Open House"** (2026-06-01).

The runtime now installed at heap creation includes:

- **Timers** — `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, and `requestAnimationFrame`. There are no OS threads behind these; the timer queue is pumped from the cooperative `WaitNextEvent` loop on every event-loop tick, so timers fire between Toolbox events rather than preempting them.
- **`window.location`** — the full surface: `href`, `protocol`, `host`, `hostname`, `port`, `pathname`, `search`, `hash`, `origin`, plus `assign`, `replace`, and `reload`.
- **`window.history`** — `back`, `forward`, `go`, `length`, and the SPA-relevant `pushState`, `replaceState`, and `state`.
- **`URL` and `URLSearchParams`** — constructed and parsed in-engine.
- **`element.classList`** — `add`, `remove`, `toggle`, `contains`.
- **`element.style`** — per-property setters.
- **DOM event constructors** — `Event`, `CustomEvent`, `MouseEvent`, `KeyboardEvent`.
- **`DOMParser`** — real (fixes873).
- **`MutationObserver`** — **stub.** The constructor exists and stores the
  callback; `observe` / `unobserve` / `disconnect` are empty and `takeRecords`
  returns `[]`. The callback is never called. Deliberate: firing records risks
  feedback loops with our own reconvert/relayout, which needs a queue-and-drain
  design before it can be safe.
- **`FormData`** — **partial.** The map (append/get/set/has/delete/forEach) is
  real, but the constructor harvests via `form.elements`, which is never defined
  on the element wrapper, so `new FormData(realForm)` always yields an EMPTY set.
- **`localStorage` / `sessionStorage`** — **in-memory shim, no persistence.**
  The method surface (getItem/setItem/removeItem/clear/key/length) is complete
  and correct, but it is a JS object with no C backing: data dies with the JS
  context, i.e. on EVERY navigation. sessionStorage has no lifetime distinct
  from localStorage. The `storage` event does not exist. (The only real,
  persistent, C-backed store in the browser is the HTTP cookie jar.)
- **`fetch`** — real since fixes846, and this entry used to UNDERSTATE it: it is
  no longer a synchronous fake thenable. `fetch()` returns a genuine QuickJS
  `Promise` wrapping a real native `XMLHttpRequest`, reaching the same
  `fetch_start()` entry point the rest of the browser uses (so cookies, UA and
  redirects all apply), with resolution deferred through the scheduler -- never
  re-entrant into JS from the fetch callback. The gaps are surface, not
  substance: `Response` carries only `ok`/`status`/`statusText`/`url`/
  `headers.get`/`text()`/`json()` -- no `blob()`, `arrayBuffer()`, `formData()`,
  no `Request`/`Response`/`Headers` constructors, no `AbortSignal`, no
  `credentials`/`mode`.
- **`addEventListener`** at the window level for `load` and `DOMContentLoaded`, alongside `getComputedStyle`, `matchMedia`, `window.scrollTo`, `navigator.userAgent`, `document.title`, and `document.readyState`.
- **Element query and geometry** — `element.matches`, `closest` and `contains`
  are real (`contains` since fixes878; it used to return a hardcoded `false`).
  **`getBoundingClientRect` is a stub** that returns all zeros unconditionally
  and never consults the box tree, so `getClientRects` and everything measuring
  geometry -- including IntersectionObserver's ratios -- reads zeros. The box
  tree itself is real; only this exposure is missing.
- **`console.log` / `warn` / `error` / `info` / `debug`**, plus `alert`, `confirm`, `prompt`, and `btoa` / `atob`.

MacSurf validates this surface with a purpose-built probe page (`mactrove.com/t.html`) that exercises each API as a numbered probe. On a G3 iMac running OS 9.2.2, that page finishes **`JS 19/19 pass, 0 fail`**. That's nineteen distinct runtime capabilities confirmed running on real 1999 hardware, not on an emulator and not on a desktop stand-in. (Emulators like SheepShaver are useful for a smoke test, but they're more forgiving than hardware; the score that counts is the one from the G3 — see [Diagnostics & Debugging](Diagnostics-and-Debugging) for why we insist on this.)

The marathon also surfaced two genuinely instructive bugs, both worth knowing if you extend the runtime. One was a stack-management mismatch: a refactor switched the per-element install helper to a no-result protected eval, but four install sites still expected the function to be left on the stack for a following `duk_call`. The result was `TypeError: [object Object] not callable` on the first use of *any* element wrapper, which aborted the install mid-stream and left every element missing `classList`, `style`, `matches`, `closest`, and friends for the rest of its life. The fix centralized eval-plus-call into one helper with correct stack discipline. The other was an event-dispatch gap: window-level listeners registered through `addEventListener('load', …)` were parked in an internal list that the event-firing path never walked — it only ran the inline `onload`-style handler — so `load` and `DOMContentLoaded` were unreachable through the standard API until the dispatcher learned to walk that list. Both are the kind of bug you only find by running real code on the target, which is the whole argument for an on-device runtime.

## The DOM bridge

The runtime would be inert if scripts couldn't touch the page. MacSurf's DOM bridge (`browser/netsurf/frontends/macos9/javascript/macsurf_js_dom.c`) connects Duktape's JavaScript objects to **libdom**, the real DOM implementation ported into the tree (the same libdom the HTML parser builds the document with — see [Architecture Overview](Architecture-Overview)).

The mechanism is straightforward once you see it. A JavaScript element object carries a hidden property — internally `__el` — holding a raw pointer to the underlying `dom_element`, stashed via Duktape's `duk_push_pointer`. JavaScript methods like `document.getElementById`, `querySelector`, `createElement`, `getAttribute`/`setAttribute`, and `appendChild` are C functions that pull that pointer back out and call into libdom. libdom is reference-counted, and the bridge respects that: a Duktape finalizer on each wrapper object calls `macsurf_dom_node_unref` (a thin wrapper that drops the underlying libdom reference via `dom_node_unref`) when the JavaScript object is garbage-collected, so DOM nodes don't leak and don't get freed out from under live JS references. That pairing — Duktape's GC on one side, libdom's refcounts on the other — is the load-bearing detail of the whole bridge.

Per-element capabilities (`classList`, `style`, `matches`, `closest`, `getBoundingClientRect`, and so on) are installed by evaluating small polyfill snippets against each new wrapper, with each install protected independently so a parse error in one shim can't take down all element wrappers. The DOM query support is real but not yet complete — `querySelector` handles the common `#id` and tag-name forms rather than the full selector grammar — and the bindings are still growing toward full libdom coverage. The direction is clear and the foundation is in place; the long tail of selector syntax and DOM methods is filled in as real pages demand it.

## The frontier — and where to track it

MacSurf runs a large amount of real-world JavaScript on hardware that predates JavaScript-heavy pages by a decade. That's the remarkable part. It's also not a 2026 desktop browser, and pretending otherwise would help no one.

The honest frontier is **heavy single-page-application frameworks** and apps that perform very large volumes of DOM mutation. Two forces work against them here: the ES5 ceiling (modern framework bundles are usually transpiled, but plenty still ship ES6+ syntax that won't parse), and raw throughput on a 233 MHz cooperative-multitasking machine, where a framework that re-renders a big tree every frame simply runs out of milliseconds. There's also a known constraint specific to the platform: JavaScript `Date` arithmetic is anchored to a fixed 2026 baseline, because Mac OS 9's `GetDateTime` returns 1904-epoch seconds with no daylight-saving handling, and the bridge to the Unix epoch is approximate.

None of this is hidden. The remaining modern JavaScript, HTML5, and CSS features the project doesn't yet cover live in the public issue tracker at [github.com/mplsllc/macsurf/issues](https://github.com/mplsllc/macsurf/issues), and that's the right place to see what's open, file what you find, or pick something up. The standing rule holds: gaps get filled in-house, on the Mac. If a site doesn't work, that's a bug to fix, not a limit to route around. If you want to help close the frontier, [Contributing & Expanding](Contributing-and-Expanding) is where to start.

## Sources

- Duktape overview, embedding model, and ES5 scope — <https://duktape.org/> and the Programmer's Guide <https://duktape.org/guide.html>
- Duktape version (2.7.0), license, and features — <https://github.com/svaarala/duktape>
- `duk_config.h` as the hand-editable portability header — <https://github.com/svaarala/duktape/blob/master/doc/duk-config.rst>
- Portability requirements (C99 recommended-not-required, byte order, alignment, known-working retro platforms) — <https://wiki.duktape.org/portability>
- MacSurf state, the JavaScript marathon, and the 19/19 G3 probe result — repo `docs/status.md` and `docs/release-notes/MacSurf-1.4.md`
- `duk_config.h` MacSurf override values, the DOM bridge, the fatal handler, and the heap/eval lifecycle — repo `browser/libduktape/duk_config.h` and `browser/netsurf/frontends/macos9/javascript/`
