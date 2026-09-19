# MacSurf Wiki — Author Brief (read this first, every time)

You are writing one page of the public MacSurf wiki. This file is the contract: voice, hard rules, canonical facts, and the full page list for cross-linking. Follow it exactly. When this brief and an old doc disagree, **this brief wins** (the old docs have drifted).

---

## Mission & audience

The wiki has three jobs, in order:

1. **Help someone build MacSurf** from a real Power Mac or an emulator — end to end, no skipped steps.
2. **Help someone understand and extend it** — architecture, the rendering/JS/networking subsystems, how to add a feature.
3. **Help someone start their own Classic Mac OS project** — so the knowledge outlives this one app.

Your reader is a **capable developer who is new to this platform**. They can program. They may never have touched Mac OS 9, CodeWarrior, the Macintosh Toolbox, or a resource fork. So: explain the platform-specific things plainly, define each piece of jargon the first time it appears — and then trust them. Don't re-explain what a compiler is. Don't say "simply" or "just." Teaching and respect are the same move here.

---

## Voice — warm, human, not robotic

Match the tone of the repo's existing `docs/cross-dev-from-linux.md`: direct, first-person-plural where natural ("we", "you"), honest about what's hard, a little dry humor. The project's own tagline is *"The modern web, on a 25-year-old Mac."* That's the spirit — affection for old hardware, no preciousness.

**Write like this:**
- Short, declarative sentences. Lead with the point.
- Explain *why* a step matters, not just what to click. ("CodeWarrior needs the `'carb'` resource to recognize the app as Carbon — without it, the binary loads as a plain classic app and crashes the moment it touches CarbonLib.")
- Name the sharp edges honestly. ("This part is genuinely fiddly. Here's the trap and here's how to step around it.")
- Second person for instructions. Active voice.

**Never write like this** (these are the AI tells that make the user's skin crawl — avoid them):
- "It is important to note that…", "It's worth mentioning…", "In this section, we will explore…", "As previously mentioned…"
- "Simply…", "Just…", "Easy!", "Don't worry!" (condescending)
- Empty hedging: "may or may not", "it depends", with no actual guidance.
- Walls of bullet points with no connecting prose. Bullets are for genuine lists; explanation goes in sentences.
- Closing "In conclusion / To summarize" recap paragraphs.
- Overuse of bold. Bold a term once when you define it; don't pepper it.

A good page reads like a knowledgeable friend who has done this many times sitting next to you — patient, specific, occasionally funny, never talking down.

---

## Markdown & wiki conventions

- GitHub-flavored markdown. Plain LF line endings (these are wiki pages, **not** Mac source — the CR rule does NOT apply here).
- One H1 (`#`) at the top = the page title. Use `##`/`###` below.
- **Cross-link generously** using the exact target filenames from the Page List below, no extension: `[Setting Up the Build Environment](Setting-Up-the-Build-Environment)`. A link to a page that another agent is writing is fine — use the listed filename.
- Code/paths/identifiers in backticks. Fenced code blocks for commands and multi-line snippets; tag the language (` ```c `, ` ```bash `).
- Tables are good for settings, version matrices, file lists. Prose is better for process.
- Start each page with one or two sentences that say what the page is for and who it's for, then get to work. No throat-clearing.

---

## HARD RULES

1. **No secrets, ever.** This is a public wiki. NEVER include any of these, in any form:
   - The proxy IP/host **`116.202.231.103`** or any `…:8765` real address. If you must show a proxy address, use the placeholder `your-proxy-host:8765`.
   - SSH keys, key filenames (e.g. `macsurf_push`), the `scp -P 2222 … localhost` transfer command, or any port-forward specifics.
   - The maintainer's personal disk/volume names (e.g. **`Back40`**), home-folder username (e.g. **`patrick`**), or email address.
   - Any private hostname, token, or internal path under `/home/…`.
   Generalize machine-specific paths to placeholders like `<YourDisk>:macsurf-source:` and explain that the leading volume name is whatever the reader's disk is called.

2. **Accuracy over fluency.** Every concrete claim about MacSurf, CodeWarrior, or OS 9 must trace to (a) the repo files you were given, (b) this brief's canonical facts, or (c) an external source you actually fetched and can cite with a URL. If you can't ground a claim, either cut it or mark it `> **TODO (verify):** …`. Do not invent file names, settings values, version numbers, or menu paths.

3. **Cite external/historical facts.** When you state something about CodeWarrior, CarbonLib, OS 9, emulators, etc. that comes from research rather than this repo, add the source URL inline or in a short "Sources" list at the page bottom.

4. **Don't oversell.** MacSurf is a young, fast-moving, nights-and-weekends project by a solo maintainer. It renders a large amount of the real web on a G3 and does native TLS 1.3 on hardware from 1999, which is genuinely remarkable — say so plainly, but don't claim parity with a 2026 desktop browser. Note real limitations where they're relevant.

5. **Respect the maintainer's boundary.** The wiki is best-effort, as-is documentation for a solo project. There is no promise of 1:1 build support. The supported build artifact is the StuffIt build pack, not a fresh `git clone`. (The Home page states this; other pages should be consistent with it and not promise hand-holding.)

6. **No proxy.** MacSurf reaches HTTPS sites natively via macTLS — there is no TLS-stripping proxy and there hasn't been for a while. Earlier versions used one; it's gone. Do not describe, diagram, recommend, or even mention a proxy anywhere in the wiki. (The `home.macsurf.org` portal is a homepage, not a proxy — don't conflate them.)

---

## Canonical facts (ground truth — use these, don't contradict them)

**What MacSurf is.** A web browser for Classic Mac OS 9 on PowerPC. It's a fork of the **NetSurf** engine with a custom Mac OS 9 frontend (the `macos9` frontend), written in C, using the **Carbon API** so one binary runs on OS 9 (via CarbonLib) and early OS X. Tagline: "The modern web, on a 25-year-old Mac." CSS3 rendering, ES5 JavaScript with a broad DOM/BOM runtime, real PNG alpha, and **native TLS 1.3** — all on a G3. It's a fast-moving solo nights-and-weekends project under the GitHub org `mplsllc`, with tagged releases (currently **v1.4**); it renders a large amount of the real web, and the long tail of modern features is tracked openly in the issue tracker.

**License.** The NetSurf core is GPLv2 (with an OpenSSL-linking exception) — see the repo `LICENSE`. Don't give legal advice; point readers to `LICENSE` for specifics.

**Current state (2026-06-01).** Latest release **v1.4 "Open House"**; the source tree is at fix round **fixes364**. Hardware-verified on a **G3 iMac running OS 9.2.2**. The full NetSurf pipeline runs natively on-Mac: fetch → HTML parse → CSS cascade (with native `var()` resolution) → layout → QuickDraw plot, with ~150 CSS properties consumed by layout. The "JavaScript marathon" (fixes319–352) closed ~23 issues and brought a large browser-runtime API surface on-device; the JS probe page scores 19/19 on a G3. Images: PNG (real alpha via lodepng), GIF, BMP, TIFF, JPEG, plus an inline-SVG V1 renderer. A companion homepage portal lives at `home.macsurf.org` (server-rendered, no-JS) — it's a convenience landing page, not part of the browser's plumbing.

**The components.**
- **Browser** (`browser/`): the NetSurf fork (`browser/netsurf/`) with the Mac frontend at `browser/netsurf/frontends/macos9/`, plus the ported NetSurf libraries: `libcss` (CSS), `libdom` (DOM), `libhubbub` (HTML5 parser), `libparserutils`, `libwapcaplet`, and `libduktape` (the JS engine).
- **macTLS**: a native TLS stack for Classic Mac OS that lets the Mac do HTTPS itself, directly over Open Transport — **there is no proxy, and hasn't been since ~2026-05-25**. It uses BearSSL for crypto primitives under a hand-written **TLS 1.3** handshake (RFC 8446: X25519 and multi-curve ECDHE key exchange, ChaCha20-Poly1305 and AES-128-GCM), with transparent fallback to TLS 1.2, and the full Mozilla CA bundle (121 trust anchors) baked into the binary. Randomness comes from **macEntropy** (a SHA-256 accumulator + HMAC-DRBG seeded from Open Transport packet jitter, event timing, a high-res clock, and a persisted seed file). Shipped in MacSurf v1.3 (2026-05-29) and is, as far as anyone knows, the **first native TLS 1.3 on Classic Mac OS**. Verified on real G3 hardware.

**Target hardware.** Power Mac G3/G4, Mac OS 9.1–9.2.2. The current project ships a **large application partition** (~195 MB preferred / ~164 MB minimum; it records `MWProject_PPC_size = 199384` / `minsize = 168192` K). **16 MB preferred / 8 MB minimum is the practical floor**, below which libcss starves mid-cascade; on a RAM-tight Mac (the 64 MB end of the range) you must lower the preferred size toward that floor or the app won't launch. CarbonLib **1.5+** to run the shipped binary; **1.6+** recommended to build.

**Build toolchain.** **Metrowerks CodeWarrior Pro 8** running *on* Mac OS 9 (PowerPC). Install the base **CodeWarrior Pro 8.0**, then apply the **8.1, 8.2, and 8.3** updaters in sequence (they are cumulative). CodeWarrior compiles the project in **C89** mode — no C99. It defines `__MWERKS__`. The project is configured as a **PowerPC Application**, creator code **`MPLS`**, file type **`APPL`** (type/creator codes are case-sensitive). C++ is off. Struct alignment is **68K** (`MC68K`). Target processor **Generic** PPC. The prefix file is **`macsurf_prefix.h`** (injected before every compile; defines `__MACOS9__`, `NO_IPV6`, `TARGET_API_MAC_CARBON`, and `#include <MacTypes.h>` first). It links the MSL runtime libraries (`MSL_C_Carbon.Lib`, `MSL_Runtime_PPC_D.Lib`) and the CarbonLib stub, and includes `MacSurf.rsrc` (which carries the mandatory `'carb'` resource + the icon family).

**Project shape (from the current CodeWarrior project, `MacSurf.xml`).** ~**850 `.c` files**, organized through **55 user access paths** + **19 system access paths**. The access paths are a **hierarchical tree** (e.g. `…:libcss:src:select:properties:`), each non-recursive except a couple — this is the current reality. (Older docs describing a single "flat folder" of files are out of date; do not repeat that.) The exact settings, access-path list, and file breakdown live in the staged facts files (see Source Map) — the `CodeWarrior Project Settings` page is built from those.

**Cross-development.** The maintainer edits on Linux and builds on the Mac. Linux is used for fast syntax-checking (a PowerPC GCC from the Retro68 toolchain, plus a plain `gcc -std=c89 -pedantic` pass with the shim include paths) before moving files to the Mac. The actual compile/link only happens in CodeWarrior on OS 9. Source files destined for the Mac need **CR (`\r`) line endings**.

**Key platform constraints (CodeWarrior 8 / C89).** No `inline`, no `//` comments, no C99 designated initializers, no `for (int i …)` loop-scope declarations, no compound literals, no variable-length arrays — all variables declared at the top of their block. Variadic macros via `__VA_ARGS__` work as a pre-C99 extension, but old-style fixed-param "varargs" macros don't. There is a real CodeWarrior PPC **`long long` / `int64_t` multiply-by-constant miscompile** — 64-bit fixed-point math is routed through `double` instead. (The full gotchas list is its own page.)

**Carbon/OS 9 essentials.** OS 9 is **cooperative multitasking** — no preemptive threads anywhere in the browser; the UI runs a `WaitNextEvent` loop and long network waits yield cooperatively. Networking uses **Open Transport** (Apple's classic TCP/IP stack) through its `*InContext` calls. A Carbon app on OS 9 **must** carry the `'carb'` resource and call `RegisterAppearanceClient()` at startup; it deliberately skips the old `InitGraf`/`InitFonts`/etc. init calls that CarbonLib handles itself.

**JavaScript.** Duktape 2.7.0, an embeddable ES5 engine, is linked into the base build and runs on the G3/64 MB floor — closures, prototypes, regex, JSON, a promise polyfill, recursion (it'll even chew through Ackermann and a Mandelbrot in pure JS). Beyond the language, a substantial browser runtime is wired on-device: DOM element/document wrappers, timers (`setTimeout`/`setInterval`/`requestAnimationFrame`), `window.location` and `window.history` (`pushState`/`replaceState`), `URL`/`URLSearchParams`, `element.classList`, `element.style`, DOM `Event`/`CustomEvent`/`MouseEvent`/`KeyboardEvent` constructors, `MutationObserver`, `DOMParser`, `FormData`, `localStorage`, `fetch`, and `addEventListener` for `load`/`DOMContentLoaded`. `duk_config.h` is hand-tuned for Mac OS 9 PPC. Heavy SPA frameworks and very large DOM-mutation apps are still a frontier (tracked openly in the issue tracker) — but there is **no proxy and no offload**: whatever runs, runs on the Mac. (Treat Duktape as capable; gaps get filled in-house, not assumed away.)

**Emulation vs hardware.** SheepShaver (a PowerPC Mac emulator) runs MacSurf and is great for a build/launch/render **smoke test**, but it is more forgiving than real hardware and its networking needs manual setup — so it is **not** a substitute for testing on a real G3/G4, especially for hardware-specific crashes. The truth always comes from real hardware.

---

## Page list (use these exact filenames for cross-links)

Nav pages (`Home`, `_Sidebar`, `_Footer`) are written by the maintainer — you may link to `Home`.

| Filename (link target) | Title | What it covers |
|---|---|---|
| `Architecture-Overview` | Architecture Overview | The whole system end to end: NetSurf engine + macos9 Carbon frontend, the ported libraries, the proxy, macTLS, how a page flows from URL to pixels. The map readers return to. |
| `The-Rendering-Pipeline` | The Rendering Pipeline | fetch → parse → CSS cascade → layout → QuickDraw plot. libcss, native `var()`, what's implemented vs parsed-but-dropped, the plotters. |
| `The-JavaScript-Engine` | The JavaScript Engine | Duktape ES5 integration, the tiers (on-device vs proxy render-and-flatten), `duk_config.h` tuning, what works and what doesn't. |
| `Networking-and-TLS` | Networking & TLS | Open Transport (the classic TCP/IP stack), the cooperative-yield fetch model, HTTP/1.1 (chunked, keep-alive, redirects), and native macTLS — how the Mac does TLS 1.3 itself, direct to origin, no proxy. The macEntropy RNG and the baked-in CA bundle live here too. |
| `Building-MacSurf` | Building MacSurf | The master build walkthrough. Two paths: (A) the StuffIt build pack — recommended, and (B) from the source tree. Build, troubleshoot, run. Links out to environment + settings pages. |
| `Setting-Up-the-Build-Environment` | Setting Up the Build Environment | Get a working Mac OS 9 + CodeWarrior Pro 8 (8.0 + 8.1/8.2/8.3) + CarbonLib, on **real hardware** and in **emulation** (SheepShaver, and notes on QEMU-PPC). Where to get things, how to install, how to verify. |
| `CodeWarrior-Project-Settings` | CodeWarrior Project Settings | The exact target settings, access-path structure, prefix file, partition size, libraries, and resource file — from the current project. Built from the staged facts. |
| `Cross-Developing-from-Linux` | Cross-Developing from Linux | The Linux-side workflow: syntax-checking with Retro68 PPC GCC + C89 gcc, the shim layer, CR line endings, and moving files to the Mac. |
| `Contributing-and-Expanding` | Contributing & Expanding | How to add a feature without breaking things: the CSS property pipeline, the library-port checklist, the regression-audit checklist, the fix/ship workflow, where to file issues. |
| `CodeWarrior-8-and-C89-Gotchas` | CodeWarrior 8 & C89 Gotchas | The hard-won landmine list: C89 restrictions, the `long long` miscompile, header/include-guard traps, union-cast issues, QuickDraw colorizing copies, struct-padding intern crashes, etc. A reference page. |
| `Diagnostics-and-Debugging` | Diagnostics & Debugging | The file-backed debug log, MacsBug basics, reading crashes (illegal-instruction/UPP/low-memory signatures), the title-bar probe technique, SheepShaver vs hardware. |
| `Start-Your-Own-Classic-Mac-Project` | Start Your Own Classic Mac Project | The reusable playbook: choosing Carbon vs classic, the toolchain options (CodeWarrior vs Retro68), resource forks & type/creator, Open Transport, a minimal Carbon skeleton, and the lessons that generalize beyond MacSurf. |
| `Resources-and-Prior-Art` | Resources & Prior Art | Curated links: NetSurf, Classilla, ssheven and other OT references, CodeWarrior/CarbonLib sources, emulators, Macintosh Garden, communities (68kmla etc.), books/docs. The jumping-off page. |

---

## Source Map (read the ones listed for your page; skim others as needed)

**Repo, in `/home/patrick/Webs/macsurf/`:**
- `CLAUDE.md` — the richest single source for **build mechanics and gotchas**: build env, Carbon/OT rules, the entire Known Gotchas list, the rendering pipeline internals, regression checklist, diagnostics. Almost every page draws on it. **BUT it is stale on _project state_:** it predates v1.x and still says v0.6.x, describes a TLS-stripping proxy and JS "proxy tiers," and frames JS as ES5-small-scripts-only. For anything about version, current capabilities, TLS, JS scope, or the proxy, **trust this brief + `docs/status.md` + `docs/version-history.md` + `release-notes/` over CLAUDE.md.** Use CLAUDE.md for *how the build and code work*, not *what state the project is in*.
- `README.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, `LICENSE`.
- `docs/architecture.md`, `docs/research/architecture-inventory.md`, `docs/research/netsurf-core-wiring.md` — architecture.
- `docs/css-status.md`, `CSS_SUPPORT_MATRIX.md`, `CSS_IMPLEMENTATION_PLAN.md` — CSS/rendering.
- `docs/codewarrior-setup.md` (note: partly stale — file counts, `WITHOUT_DUKTAPE`, "flat folder" are out of date; mine it for the install/transfer mechanics, not the numbers), `builds/README.md`.
- `docs/cross-dev-from-linux.md` — the Linux workflow (good voice reference, still accurate).
- `docs/status.md`, `release-notes/` — **current state** (version, TLS 1.3, macEntropy, JS marathon). Authoritative for "where the project is now."
- macTLS / TLS details also appear in the memory notes the maintainer keeps; the networking page author should lean on `docs/status.md` and the `release-notes/MacSurf-1.3*.md` notes for the TLS 1.3 story. (Do **not** document the old proxy — it's gone.)
- `docs/version-history.md`, `docs/changelog-fixes.md`, `docs/status.md` — history/state.
- `docs/usb-overdrive.md`, `docs/research/window-architecture-2026-04-22.md`, `docs/research/state-survey-2026-04-18.md` / `…-19.md` — frontend internals.
- `docs/resources.md` — resource-fork / `'carb'` pipeline.

**Staged exact facts, in `/home/patrick/Webs/macsurf/wiki/.facts/`:**
- `01-scalar-settings.txt` — exact CodeWarrior target settings (linker, project type, creator/type, partition, prefix, alignment).
- `02-prefix-text.txt` — (may note the prefix is a file, not inline).
- `03-access-paths.txt` — the 55 user + 19 system access paths (verbatim, with the maintainer's volume/username — **generalize these, never publish `Back40`/`patrick`**).
- `04-file-list.txt` — the ~850-file project list with extension breakdown.

**Research notes, in `/home/patrick/Webs/macsurf/wiki/.facts/research/`** (written by the research phase; read the ones relevant to your page): `codewarrior-pro-8.md`, `carbonlib.md`, `os9-emulation.md`, `os9-real-hardware.md`, `netsurf-engine.md`, `classic-mac-dev.md`, `file-transfer-os9.md`, `duktape-embedding.md`.
