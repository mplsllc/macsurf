# Diagnostics & Debugging

This page is about seeing what the Mac is actually doing. MacSurf runs on hardware from 1999 that has no console, no `stderr` you can tail, and often no debugger attached. So the project leans on a handful of low-tech channels — a file on the Desktop, the window title bar, a few `about:` pages, and (when you have it) MacsBug. This page walks through each one, what it's good for, and how to read the crash signatures that come out the other end. It's written for someone extending MacSurf or debugging their own Classic Mac code; you don't need prior OS 9 experience, but you should be comfortable reading C.

The hard reality up front: OS 9 is cooperative multitasking, so a hung MacSurf can take the whole machine down with it, and a crash often leaves you staring at a frozen screen or a reboot. The whole point of the tooling below is to leave a durable trail *before* that happens, so you can reconstruct what the code was doing in its last moments.

## The file-backed debug log

This is the primary post-crash channel, and the one you'll reach for most. MacSurf writes a plain text file called `MacSurf Debug.log` to the Desktop, one line per log call, and flushes it to disk so the line survives even if the machine dies a microsecond later.

The reason it exists is simple: the dev hardware (a G3 iMac) is USB-only, with no ADB keyboard, so MacsBug's interactive commands aren't always reachable, and a frozen Mac can't print anything anyway. A file that's already on disk when the crash hits sidesteps all of that. After a crash, you reboot, open `MacSurf Debug.log` in SimpleText, and the last few lines show the code path that was executing when the Mac went down. That's your back trace.

The API lives in [`macsurf_debug_log.h`](https://github.com/mplsllc/macsurf/blob/master/browser/netsurf/frontends/macos9/macsurf_debug_log.h):

```c
void macsurf_debug_log_init(void);                 /* at startup */
void macsurf_debug_log_close(void);                /* at shutdown */
void macsurf_debug_log_write(const char *msg);     /* literal string */
void macsurf_debug_log_writef(const char *fmt, ...);/* minimal printf */
void macsurf_debug_log_flush(void);                /* force a volume flush */
```

In practice you don't call these directly very often. The macro you use is `MS_LOG`:

```c
MS_LOG("entering html_reformat");
```

`MS_LOG(literal)` is exactly equivalent to `macsurf_debug_log_write` — use it for fixed strings and you get the durable record for free. When you need values interpolated, reach for `macsurf_debug_log_writef`.

### The formatter only knows five specifiers

`macsurf_debug_log_writef` does **not** use the MSL C library's `vsnprintf` — that routine is unreliable under CodeWarrior's Carbon MSL. It's a hand-rolled formatter, and it understands exactly five things:

| Specifier | Meaning |
|---|---|
| `%d` | `int`, signed decimal |
| `%ld` | `long`, signed decimal |
| `%p` | pointer, 8 hex digits, no `0x` prefix |
| `%s` | `const char *` (`NULL` prints as `(null)`) |
| `%%` | a literal percent sign |

Anything else — `%u`, `%x`, `%f`, `%zu` — is **echoed verbatim and does not consume its argument.** That last part is the trap: an unrecognized specifier doesn't just print wrong, it throws off the argument alignment for every `%s`/`%d` that follows it, and a misaligned `%s` will happily dereference a garbage pointer and crash. If your log line prints `%u` literally in the output, that's the bug — replace it with `%d` or `%ld`. Output is hard-capped at 255 bytes, so there's no overflow risk, but long lines get truncated.

### Why it survives crashes (and the one time it won't)

Each `macsurf_debug_log_write` appends the line to an in-memory buffer, and the channel flushes that buffer to physical disk when it fills, on close, or on an explicit flush call (fixes261). Early versions flushed the volume after *every* line, which was correct but brutal: an HFS volume sync costs roughly 10–50 ms, and a single page load fires ~80 log calls, so you were paying one to four seconds of pure disk wait per page. The current design (fixes96) drops the per-line flush and exposes `macsurf_debug_log_flush()` so you can force a sync only after the messages that genuinely must survive — right before entering a code path you suspect is about to crash, for instance. A clean shutdown through `macsurf_debug_log_close()` flushes implicitly, so normal exits lose nothing.

The one failure mode: the channel depends on HFS actually committing the write. If the target volume is full, dismounted, or read-only, the write silently fails. If you find the log file exists but is truncated or stale after a crash, the filesystem journal didn't catch up — retry on a different volume or add a delay after the critical write. And if `FindFolder` can't locate the Desktop at init time, the log goes silently inert: `init` won't crash, but no file appears and every subsequent call is a no-op. An *absent* `MacSurf Debug.log` is itself a diagnostic — it usually means init never ran.

### The MACSURF_DEBUG gate — and the regression it caused twice

The entire log implementation is wrapped in `#ifdef MACSURF_DEBUG`. Without that define, `macsurf_debug_log_init`, `macsurf_debug_log_write`, and the `MS_LOG` macro all compile down to empty stubs — the symbols still exist so the link succeeds, but nothing is ever written. This is deliberate: release builds (with `MACSURF_RELEASE` set) ship the channel as no-ops so end users don't get a log file growing on their Desktop.

That gate has bitten the project twice, and both times the symptom was identical and maddening: instrumentation was added across several fix rounds, every round "landed," and *zero* log output appeared.

- The first time, `macsurf_debug_log_init()` was never called from `main()`. The writes short-circuited silently because the channel was never open.
- The second time, `MACSURF_DEBUG` itself went missing from the prefix file during a long sprint. The init call was still wired, but with the define gone the whole body — init, write, and every `MS_LOG` call site — compiled out to the release stub.

The lesson, now baked into the project's [regression-audit checklist](Contributing-and-Expanding), is that "the code compiled and the init call is present" is not enough. Two things have to be true: the init function is actually called from `main()`, **and** its real body isn't hidden behind a feature macro that's never defined. The fix for the second case was to add `#define MACSURF_DEBUG 1` (gated on `#ifndef MACSURF_RELEASE`) to `macsurf_prefix.h`, and to add an audit step that greps the prefix file for the define. If you're wiring up your own debug channel on a fresh project, take this as a free lesson: gate it on a macro by all means, but check the macro is defined where you think it is.

## The title-bar probe

The window title bar is a second, *live* channel. You can shove a string into it and watch it change in real time as code runs — handy for pipeline progress you want to see without rebooting to read a file.

The macro is `MS_LOG_STICKY`:

```c
MS_LOG_STICKY("cascade done, entering layout");
```

This calls `macsurf_debug_set_title_force`, which overwrites the front window's title. There's a matching `macsurf_debug_log_int_force(label, value)` for dropping a number up there.

A bit of history worth knowing, because the macros changed under it: plain `MS_LOG` used to be *dual-channel* — it wrote the file **and** the title bar. That turned out to be a mistake. Every trace string ("gui inv", "resize done", and so on) clobbered the real page title, so the actual title of the page you were looking at became invisible, and internal debug strings leaked into the chrome the user sees. As of fixes167, `MS_LOG` is file-only. If you want something in the title bar deliberately, that's what `MS_LOG_STICKY` is for. (`MS_BREAK` and `MS_ASSERT` still hit both channels, because they only fire on real failures and you want those surfaced immediately.)

### The clobber pitfall

The title bar holds exactly one string. Last writer wins. If two different probes both call `macsurf_debug_set_title_force` during the same redraw or reformat cycle, you only ever see the second one — the first is gone before your eyes can register it. This produces a nasty false signal: you add a probe, it never shows, and you conclude the code path is dead, when in fact a *later* probe in the same cycle is stomping it a few microseconds later.

So when you add a sticky probe, **strip the old stickies first.** Two indicators help you read the title bar honestly. If the title is cycling between different labels on its own (say it flips between "plot rect" and "plot clip"), that means *no* probe is latched and you're seeing live `MS_LOG`-style updates — which usually means the sticky path you expected to fire is dead. If the title is frozen on one string, something has latched it; make sure it's the probe you think it is and not a leftover from three rounds ago.

A related gotcha around shipping: a fix bundle only refreshes the files it actually contains. If you added a probe to `window.c` in one round and the next bundle doesn't include `window.c`, that probe is still on the Mac even though you deleted it from the source on your Linux box. The symptom is phantom probe output with no matching line in your source — which means the on-Mac copy of that file is out of sync. Ship the file explicitly to resync. (More on the build/ship workflow in [Cross-Developing from Linux](Cross-Developing-from-Linux).)

## The about: diagnostic pages

MacSurf renders four internal diagnostic pages. Type any of them into the address bar — they're real rendered pages, not dialogs, styled with the same Geneva chrome and orange-banded tables.

| URL | What it shows |
|---|---|
| `about:cache` | the on-disk body cache shape and the in-memory cache state |
| `about:memory` | the application partition and memory footprint |
| `about:config` | a snapshot of key NetSurf options (V1 is a fixed read-only subset — `enable_javascript`, `foreground_images`, `background_images`, `author_level_css`, `max_fetchers`, `memory_cache_size` — not yet the full live table) |
| `about:perf` | live performance counters, including `reformat_ms` (the time the last layout reformat took, captured in `html_reformat`) |

`about:perf` is the one to watch when a page feels slow. A modern page can fire well over a dozen full reformats during a single load — one for every CSS file and every image that arrives — and each one walks the box tree. If `reformat_ms` is high and reformats are frequent, layout is your bottleneck, not the network.

These pages came with a subtlety worth flagging if you're touching URL handling. For a long time they didn't load at all: the URL-bar code guessed at a missing scheme using `strstr(url, "://")`, which only matches *hierarchical* schemes like `http://`. Opaque schemes — `about:`, `data:`, `javascript:`, `mailto:`, `file:`, `resource:` — have the form `scheme:opaque` with no `//`, so `about:cache` got rewritten to `https://about:cache`, parsed as host `about` / path `cache`, and 404'd to a blank page. The fix replaced the heuristic with a proper RFC 3986 scheme scan, which is why all six opaque schemes now work from the address bar. If you add a new opaque scheme and it mysteriously becomes `https://yourscheme:...`, that scanner is where to look.

## Capturing what's on screen

When a bug is *visual* — a misrendered page, a layout that's subtly off, a glitch you want to attach to an issue — describing it in words rarely does it justice. OS 9's built-in capture (Cmd-Shift-3) writes a clunky `Picture` file to the disk root and stops there. [Snapz Pro 2](https://macintoshgarden.org/apps/snapz-pro-2) is the screen grabber the maintainer reaches for instead: it captures a region or a window to a standard image you can move off the Mac and drop straight into a bug report or compatibility thread. (A *MacsBug* screen lives outside the GUI, so a phone photo of the monitor is still the realistic way to capture that one.)

## MacsBug — the real debugger

[MacsBug](https://en.wikipedia.org/wiki/Macsbug) is Apple's classic low-level assembly debugger ([download from Macintosh Garden](https://macintoshgarden.org/apps/macsbug)). It installs into the System Folder and, when an unhandled exception fires, it drops you out of the GUI into a text-mode debugger where you can read the program counter, the registers, and a stack trace, then disassemble around the crash. When you have a keyboard that can reach it, it's far more powerful than the log file — the log tells you the last *checkpoint* you passed, MacsBug tells you the exact *instruction* that died.

> **TODO (verify):** MacsBug requires an interrupt to drop in (historically the programmer's switch / NMI, or the keyboard interrupt) and a keyboard it can read. On the project's USB-only G3 iMac, getting an interactive MacsBug session has been a recurring obstacle and some hardware crash investigations were deferred for lack of a usable keyboard path. If you're documenting a specific key combination to invoke MacsBug on a given machine, confirm it against that hardware rather than trusting folklore.

### A short command cheat sheet

MacsBug drops you at a `>` prompt with no menus and no hints, which is unnerving the first time. You type a command, press Return, and read what comes back. The handful that actually matter:

*Get oriented:*
- `how` — why you're here: the kind of exception that broke you in (bus error, illegal instruction, and so on).
- `td` — "total display," redraw the register window (the PC, the general registers, the condition register).

*Look around:*
- `wh <address>` — "where": the nearest symbol to an address, so a raw crash address turns into a function name. This is the one behind every "we need a `wh` stack capture" request — drop in, run `wh` on the crash address, and read off the symbol.
- `sc` — "stack crawl": the chain of calls that led to the crash. The closest thing to a back trace.
- `ip` — disassemble around the instruction pointer (the PC), so you can see the exact instruction that died and the few around it.
- `dm <address>` — display memory at an address; handy for peering at a struct or a buffer.
- `esc` — peek at the screen that was showing when MacsBug grabbed control; press `esc` again to return. Good for "what was on screen when it died."

*Get out again:*
- `g` — "go": leave MacsBug and resume execution where it broke. Use this once you've looked, when the program can safely carry on.
- `es` — "exit to shell": force the crashed app to quit and return to the Finder. The graceful escape when resuming isn't safe but the machine is otherwise fine. (`ea` does the same force-quit.)
- `rs` — restart the machine in software. Reach for this when `es` can't recover but you'd rather not hit the power switch.
- `rb` — reboot, a harder restart than `rs` for when the Mac is well and truly wedged.

You won't memorize MacsBug from this page, and you don't need to — these dozen commands cover nearly everything MacSurf debugging asks of it. The full reference is Apple's *Debugging Macintosh Software with MacsBug* (linked in Sources).

### Reading MacSurf's crash signatures

Years of crashing this thing on real hardware have produced a small catalog of recognizable signatures. Learning to read them saves enormous time, because the crash *address* often tells you the *category* of bug before you've looked at a single line of code.

**Illegal instruction at a heap-looking address.** The crash PC is somewhere that looks like a data address — a big number that isn't in any code segment. This almost always means execution jumped through a bad function pointer: the code called *through* something that wasn't actually a routine, and the CPU started interpreting data bytes as instructions until it hit one it couldn't decode. A classic real-world instance was the mouse-wheel handler: `kEventMouseWheelMoved` was never back-ported to CarbonLib on OS 9, so registering a handler for it left CarbonLib's dispatcher walking uninitialized state and executing garbage. The handler code looked perfectly correct in review — the bug was that the *platform* couldn't deliver that event class at all. The takeaway: an illegal-instruction-at-heap-address crash points you at "who called through a pointer that wasn't a valid routine," not at the arithmetic on the line above.

**PC in very low memory, with `bl 0` / the UPP signature.** If the program counter is at an address like `0x00000008` and the link register is at `0x00000004`, you're looking at a botched [Universal Procedure Pointer (UPP)](https://orangejuiceliberationfront.com/universal-procedure-pointers/). Under CarbonLib on OS 9, system calls that take a UPP (like `TrackControl` or `InstallEventHandler`) expect a real *routine descriptor* — a small structure the Mixed Mode Manager reads to find the actual code — **not** a raw function pointer. (On Mac OS X's Carbon.framework a UPP *is* just a function pointer, which is exactly why the "optimization" of casting straight to a UPP looks correct and then crashes only on OS 9.) When you hand Mixed Mode a raw pointer where it expected a descriptor, it reads descriptor fields out of your function's first bytes, resolves a target address of 0, and executes a branch-and-link to address 0 — which sets LR to 4 and sends the CPU walking forward through low-memory globals until the first illegal opcode. The tell-tale: PC and LR both in single-digit-hex low memory. A frequent companion clue is that `CurApName` reads `CodeWarrio…`, because CodeWarrior's runtime grabbed the low-memory trap. The fix is never to debug the "handler" — it's to stop casting function pointers to UPPs and let the Universal Interfaces build the routine descriptor, or to avoid the action-UPP path entirely. (This and the rest of the landmine list live on the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page.)

**The same instruction crashes every time, somewhere inside OTClientLib.** If you get a deterministic crash at a fixed address deep inside Open Transport's client library, before suspecting your networking code, check that the binary is recognized as a Carbon fragment. A Carbon app on OS 9 **must** carry the `'carb'` resource. Without it, CFM loads the binary as a plain classic PEF, CarbonLib never engages as a dependency, and the first `*InContext` Open Transport call enters an uninitialized CarbonLib client context and crashes at a fixed spot every time. This is the single most common "it crashes instantly and identically" cause, and it has nothing to do with your logic — see [CodeWarrior Project Settings](CodeWarrior-Project-Settings) for how the `'carb'` resource gets into the build, and [Networking & TLS](Networking-and-TLS) for the Open Transport side.

The general habit these signatures train: read the *address* first. A heap-looking PC, a low-memory PC, and a fixed OTClientLib PC are three completely different bug families, and the address tells you which family you're in before you open the source.

## SheepShaver versus real hardware

[SheepShaver](https://sheepshaver.cebix.net/) is a PowerPC Mac emulator that runs OS 9 and runs MacSurf. It's genuinely useful for the right job and genuinely misleading for the wrong one, so it's worth being precise about the line.

Use SheepShaver for **smoke tests**: does the build launch at all, does Carbon initialize, do the CarbonLib and Open Transport dependencies resolve, does a known page still render, did a layout change regress something obvious. It catches compile-clean-but-broken mistakes — toolbox misuse, a link that succeeds but does the wrong thing — that a Linux syntax check can't. For confirming "I didn't break rendering," it's quick and worth doing on every build.

Do **not** trust SheepShaver for hardware-specific crashes. Its CarbonLib and Control Manager emulation is more forgiving than a real G3 or G4 — code paths that crash on metal run clean in the emulator. The scroll-bar-click crash is the canonical example: the live-tracking Appearance scroll-bar CDEF crashed into MacsBug on real hardware while running perfectly in SheepShaver. A green light in the emulator does **not** mean the real machine is happy. The same caution applies to anything timing-sensitive (the JIT and the cooperative scheduler pace differently from real PPC), to real network behavior (the emulator's Open Transport needs manual setup to reach the internet and otherwise blocks until timeout), and to anything involving third-party trap patches like USB Overdrive, which don't exist in the emulator.

The rule that's held up: SheepShaver tells you the build is *not broken*; only a real G3/G4 tells you it *works*. When a bug smells hardware-specific — a crash on scroll, on the wheel, anything that touches CarbonLib internals or device drivers — move to real hardware and capture a MacsBug `wh` trace. Everything else, smoke-test in the emulator first to save the round-trip. There's more on getting an emulator and real hardware stood up in [Setting Up the Build Environment](Setting-Up-the-Build-Environment).

## Putting it together

A practical debugging session usually layers these. You sprinkle `MS_LOG` checkpoints through the code path you're chasing so the file log captures how far execution got. You drop an `MS_LOG_STICKY` in the one spot you want to watch live (after stripping any old stickies). You smoke-test in SheepShaver to confirm the build still launches and renders. Then you run it on the real G3, and if it crashes, you read the last lines of `MacSurf Debug.log` to see the last checkpoint passed, and — if you can reach it — read the MacsBug crash address to classify the bug. The log tells you *where in the pipeline*; the address tells you *what kind of failure*; together they usually point straight at the line.

## Sources

- MacsBug — concept: https://en.wikipedia.org/wiki/Macsbug ; download: https://macintoshgarden.org/apps/macsbug
- MacsBug command reference — Apple, *Debugging Macintosh Software with MacsBug* (1991): https://vintageapple.org/macbooks/pdf/Debugging_Macintosh_Software_with_MacsBug_1991.pdf
- Snapz Pro 2 (screen capture for OS 9) — https://macintoshgarden.org/apps/snapz-pro-2
- Universal Procedure Pointers / Mixed Mode Manager — https://orangejuiceliberationfront.com/universal-procedure-pointers/
- Carbon API and CarbonLib availability — https://en.wikipedia.org/wiki/Carbon_(API)
- SheepShaver (PowerPC Mac emulator) — https://sheepshaver.cebix.net/
- MacSurf source: `browser/netsurf/frontends/macos9/macsurf_debug_log.h`, `macsurf_debug.h` (in the repo).
