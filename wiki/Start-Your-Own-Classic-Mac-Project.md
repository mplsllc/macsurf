# Start Your Own Classic Mac Project

This page is a playbook for the developer who reads about MacSurf and thinks, "I want to build my own thing for this machine." Maybe a text editor, a chat client, a game, another browser. You can program; you've just never shipped anything for Mac OS 9. We'll use MacSurf as the worked example throughout, because every decision here was made for real and the scars are instructive — but the goal is to hand you a pattern you can take anywhere on the platform, not to make you build a browser.

A short, honest note before we start: MacSurf is a solo nights-and-weekends project, and so is most of what survives in this corner of computing. The good news is that the platform is small and finite. Nobody is changing the Toolbox out from under you. Once you understand the handful of rules that make a Carbon app come alive on OS 9, you understand them forever.

## First decision: Carbon or the classic Toolbox?

The Macintosh Toolbox is the set of system routines — originally baked into ROM — that gave every classic Mac its windows, menus, events, and resource handling, all the way from 1984 through Mac OS 9 ([retrotechcollection wiki](https://wiki.retrotechcollection.com/Macintosh_Toolbox)). When you write a native OS 9 app, you are calling the Toolbox. The real question is *which dialect* of it you call.

You have two:

**The classic Toolbox** is the original API. You poke window and event structures by their fields directly, you call `GetNextEvent` in a polling loop, and you can reach all the way down to interrupt handlers and device drivers. It runs on System 7 through Mac OS 9, and only there.

**Carbon** is a cleaned-up C API, introduced in 2000, that Apple designed so one source base could run on both classic Mac OS and Mac OS X ([Carbon (API), Wikipedia](https://en.wikipedia.org/wiki/Carbon_(API))). It covers roughly 95% of the calls real applications actually make ([Apple, *Carbon Porting Guide*](https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf)). The trade-off is that most data structures become *opaque* — you go through accessor functions instead of touching fields — and a Carbon app can no longer install interrupt handlers or drivers. On OS 8/9, Carbon is implemented by a single system extension called **CarbonLib**; on OS X the same role is played by `Carbon.framework`.

For a new project, **choose Carbon.** Two reasons. First, the opaque-struct discipline is good for you — it's the same discipline a modern API would impose, and it keeps your code from depending on internal layouts that were never stable. Second, a Carbon binary runs unchanged on OS 9 *and* early PowerPC Mac OS X, which roughly doubles the set of machines that can run your work for free. MacSurf is Carbon for exactly this reason: one binary that runs on OS 9 and early PowerPC Mac OS X.

Reach for the classic Toolbox only if you genuinely need something Carbon dropped — direct hardware access, a device driver, an INIT/extension — or if you're targeting machines older than the CarbonLib floor (Carbon needs OS 8.1+). For most applications, that's not you.

## The toolchain: CodeWarrior on-Mac, or Retro68 cross-compile?

There are two realistic ways to turn C into a PowerPC Carbon binary, and they sit at opposite ends of a comfort-versus-period-authenticity spectrum.

### CodeWarrior Pro 8 — building on the Mac itself

Metrowerks CodeWarrior Pro 8 is the IDE-and-compiler that the platform actually shipped with. It runs *on* a PowerPC Mac under OS 9, and it produces PEF executables — the PowerPC code container that the Code Fragment Manager loads ([Preferred Executable Format, Wikipedia](https://en.wikipedia.org/wiki/Preferred_Executable_Format)). This is what MacSurf builds with, and it's almost certainly the most battle-tested path for serious work, because nearly every commercial OS 9 app of the era went through it.

What you should know going in:

- It compiles in **C89 mode** — no C99. That constraint shapes everything you write (more on this below).
- Installation is a chain, not a single package: install the base **Pro 8.0**, then layer the **8.2** updater, then **8.3**. The 8.1 step is optional because 8.2 accepts an 8.0 base, but 8.3 will refuse to apply unless 8.2 is already present ([Macworld on the 8.2 update](https://www.macworld.com/article/155573/codewarrior-5.html); [Macworld on 8.3](https://www.macworld.com/article/156765/codewarrior-6.html)).
- It's archived today on the [Macintosh Garden](https://macintoshgarden.org/apps/codewarrior-pro-8x) and the [Macintosh Repository](https://www.macintoshrepository.org/1351-codewarrior-pro-8-x), since Metrowerks is long gone.

The full install-and-configure walkthrough lives on the [Setting Up the Build Environment](Setting-Up-the-Build-Environment) and [CodeWarrior Project Settings](CodeWarrior-Project-Settings) pages — read those when you're ready to actually build.

### Retro68 — cross-compiling from a modern machine

[Retro68](https://github.com/autc04/Retro68) is a modern GCC-based cross-compiler that targets 68K *and* PowerPC classic Macs, including Carbon. It bundles a recent GCC, a `Rez` resource compiler, and the `MakePEF` tool that turns the linker's output into a CFM-loadable PEF binary. It runs on your Linux or macOS laptop and emits Mac binaries — no period hardware required to compile.

The appeal is obvious: a fast modern host, real version control, an editor you already like, and no fighting a 25-year-old IDE. The catch is that the *language* it accepts and the language CodeWarrior accepts are not the same. Retro68's GCC targets C++17 by default and is happy with C99; CodeWarrior is strict C89. Code that compiles clean under Retro68 can still fail under CodeWarrior, and the reverse happens too.

MacSurf's answer is to use **both, for different jobs**. The maintainer edits and develops on Linux, and uses Retro68's PowerPC GCC purely as a fast *syntax pre-flight* — a `-std=c89 -pedantic-errors` pass to catch C99-isms and obvious mistakes before a file ever moves to the Mac. The actual compile and link that produces the shipping binary happens in CodeWarrior on OS 9, because that's the toolchain the binary is verified against. The full cross-development loop is documented on [Cross-Developing from Linux](Cross-Developing-from-Linux).

For your own project the honest guidance is: if you want the smoothest modern experience and don't need byte-for-byte CodeWarrior fidelity, Retro68 alone is a fine and complete toolchain — plenty of people ship with it. If you're matching an existing CodeWarrior codebase or you want the exact toolchain the platform shipped with, build on the Mac and use Retro68 as your fast feedback loop. Either way, decide which compiler is your *source of truth* and write to that one's rules, because straddling both without a clear primary is how you collect mystery breakage.

## A minimal Carbon app skeleton

Here is the smallest thing that counts as a real Carbon app on OS 9: it initializes correctly, it's recognized as Carbon, and it runs an event loop that yields the CPU like a good citizen. Everything else — windows, menus, drawing — hangs off this frame.

The startup sequence on Carbon is *shorter* than on the classic Toolbox, and that's the first surprising thing. Under the classic Toolbox you'd open with a litany of `InitGraf` / `InitFonts` / `InitWindows` / `InitMenus` / `TEInit` / `InitDialogs` calls. Under Carbon you **skip all of them** — CarbonLib initializes those managers itself, and calling them yourself is at best redundant and at worst destabilizing. What you keep is small:

```c
int main(void)
{
    /* 1. Cursor + flush the event queue of anything left over. */
    InitCursor();
    FlushEvents(everyEvent, 0);

    /* 2. Tell the Appearance Manager we're a client, so controls and
     *    windows draw with the platinum theme instead of the old
     *    System 7 look. Gate on a Gestalt check so we don't call it
     *    on a system that lacks Appearance. */
    {
        long response;
        if (Gestalt(gestaltAppearanceAttr, &response) == noErr) {
            RegisterAppearanceClient();
        }
    }

    /* 3. Open your window, install handlers, set up state here. */

    /* 4. The cooperative event loop. */
    {
        EventRecord ev;
        Boolean done = false;
        while (!done) {
            /* WaitNextEvent yields the CPU to other processes while
             * we have nothing to do. The third argument is the sleep
             * quantum in ticks (1 tick = 1/60 s); a small value keeps
             * us responsive, a larger one is friendlier to the system
             * when idle. */
            if (WaitNextEvent(everyEvent, &ev, 30L, NULL)) {
                switch (ev.what) {
                    case mouseDown:   /* dispatch to your handlers */ break;
                    case keyDown:     break;
                    case updateEvt:   break;
                    case kHighLevelEvent:
                        /* AppleEvents arrive here; the Quit AppleEvent
                         * is how a well-behaved app is told to exit. */
                        AEProcessAppleEvent(&ev);
                        break;
                    /* ... */
                }
            }
        }
    }
    return 0;
}
```

A few things in that frame are doing real load-bearing work, and they're worth understanding rather than copying blind.

**`RegisterAppearanceClient()`** opts you into the Appearance Manager, which is what makes your controls and window frames render in the OS 8/9 "platinum" theme instead of the flat black-and-white System 7 look. MacSurf calls it right after `InitCursor()`, gated on a Gestalt check, mirroring the pattern from Classilla (the Mozilla-era OS 9 browser). The Gestalt gate matters because the call isn't safe on a system that never shipped Appearance.

**`WaitNextEvent`** is the heart of the whole thing, and it deserves its own section.

### The cooperative-multitasking mindset

This is the single biggest shift coming from a modern OS, so internalize it now. Classic Mac OS is **cooperative**, not preemptive: each running application must *voluntarily* hand the processor back so another can run ([Apple, *Multitasking* concepts](https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html)). There is no scheduler that will interrupt you. If your code goes into a long loop and never yields, it doesn't freeze your app — **it freezes the entire machine.** The clock stops, the cursor stops, the user reaches for the power switch.

`WaitNextEvent` is how you yield. When you call it and there's nothing for you to do, it grants CPU time to background processes until an event for you arrives ([Mike Ash, *The Mac Toolbox followup*](https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html)). So the entire shape of a classic Mac program is: *do a small bit of work, return to the event loop, do a small bit more.* Long tasks get broken into chunks that each finish quickly, or they're structured to yield partway through.

For MacSurf this rule is everywhere. There are **no preemptive threads anywhere in the browser** — they don't exist on the platform, and pretending otherwise just crashes. Network fetches, which can block for seconds, can't be allowed to stall the loop, so they yield cooperatively (we'll get to how in a moment). When you design your own app, ask of every operation: *can this run long enough that the user would notice the machine stop?* If yes, it needs to yield.

### The mandatory `'carb'` resource

Your beautifully-initialized Carbon app will still crash on launch if you forget one thing, and it's an easy thing to forget because it isn't code at all. It's a resource.

A Carbon CFM binary must carry a **`'carb'` resource at ID 0** (or its newer sibling `'plst' 0`). This resource is the marker that tells the Code Fragment Manager "this fragment is Carbon," which is what causes CarbonLib to load as a dependency at launch ([Apple, *Carbon Porting Guide*](https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf)). Its *contents* don't matter — CarbonLib reads only its presence, so it's typically zero bytes.

Leave it out and the failure mode is vicious and non-obvious: the binary loads as a plain classic PEF, CarbonLib never engages, and the first Carbon call into an uninitialized CarbonLib context crashes — for MacSurf, that surfaced as a crash at a fixed address deep inside Open Transport's client library. If you ever see the *same* instruction crashing every launch somewhere inside a system library, suspect the missing `'carb'` marker before you debug anything else. (Note that `'carb'` is distinct from the `'cfrg' 0` resource, which is the CFM's map of where your code and imported libraries live — a CFM Carbon app needs both.)

MacSurf ships these in a pre-built binary resource fork, `MacSurf.rsrc`, which CodeWarrior links straight into the output with no Rez step. That same fork also carries the app icon family and the `BNDL`/`FREF` resources — which brings us to the next platform concept you can't skip.

## Resource forks and type/creator codes

Here is the thing that trips up everyone arriving from Unix or Windows: on classic Mac OS, **a file is two files in a trench coat.** Every file has a *data fork* (an ordinary unstructured byte stream) and a *resource fork* (a structured, database-like store of typed records — icons, menus, dialogs, and on classic Mac OS, the application's executable code itself) ([Resource fork, Wikipedia](https://en.wikipedia.org/wiki/Resource_fork)).

This has two consequences you must respect.

The first is about **moving files around.** Resource forks only survive on Apple filesystems (HFS, HFS+). Copy a Mac application to a FAT USB stick or push it through a naive network transfer and the resource fork is silently dropped — which, since the code lives in that fork, leaves you with a dead husk. This is *the* classic "my app won't launch and I don't know why" mistake. The way around it is to wrap the file in a format that carries both forks: a StuffIt `.sit` archive, a BinHex `.hqx` text encoding, or an HFS disk image, or to share over AFP (e.g. via netatalk) which preserves the metadata. The short version: *never transfer a bare Mac binary over a foreign filesystem and expect it to live.* MacSurf's own ship workflow exists precisely to keep forks intact end to end.

The second is about **file identity.** The classic Finder doesn't key off filename extensions. It keys off two four-byte codes stored in the file's Finder info: a **type code** describing what kind of file it is, and a **creator code** identifying the app that owns it ([Creator code, Wikipedia](https://en.wikipedia.org/wiki/Creator_code)). An application's type code is always `APPL`. The creator code is yours to pick — MacSurf's is `MPLS`. These codes are **case-sensitive**: `MPLS` and `mpls` are different creators, and getting the case wrong orphans your icon and your documents. (Apple historically reserved all-lowercase codes for itself, so an uppercase code is the conventional choice for third parties.)

The icon binding works through two more resources in your fork: a `BNDL` ("bundle") that maps your creator code to an icon and the file types you handle, and `FREF` ("file reference") records describing each type. The Finder reads the `BNDL` during a desktop rebuild and remembers "creator `MPLS` → this icon," after which your app shows its real icon. If your freshly-built app shows the generic application icon, rebuild the desktop (hold Command-Option at boot) and confirm the `BNDL`/`FREF`/type/creator are all correct. MacSurf generates this whole fork from a single PNG with a Python script — see the repo's `docs/resources.md` for that pipeline — but the *concepts* (type, creator, BNDL, FREF) are the same whether you generate them or hand-build them in ResEdit.

## Networking with Open Transport

If your app talks to the network, you'll be using **Open Transport** — Apple's classic TCP/IP (and AppleTalk) stack, built on System V STREAMS, introduced in 1995 and built into the OS from Mac OS 8 onward ([Open Transport, Wikipedia](https://en.wikipedia.org/wiki/Open_Transport)). It replaced the older, slower MacTCP. There's no BSD sockets layer here; OT has its own endpoint-based API.

The interesting design problem is the collision between two facts: network calls block, and you're not allowed to block the machine. Open Transport's answer, and MacSurf's, is **synchronous calls with cooperative idle events.** You set `OTUseSyncIdleEvents(endpoint, true)` and install a notifier callback. While a synchronous OT call is waiting on the network, OT periodically fires a `kOTSyncIdleEvent` into your notifier — and your notifier responds by calling `YieldToAnyThread()` (from the classic Thread Manager). The effect is that a "blocking" fetch politely hands the CPU around the rest of the system on every idle tick, so the machine stays alive even mid-download. That's the cooperative-multitasking mindset applied to I/O, and it's the pattern to copy.

For addressing, `OTInitDNSAddress(&addr, "host:port")` takes a single string and lets OT resolve both the hostname and the port, which is simpler than the older two-step host-then-port dance. For an outbound-only TCP client, `OTBind(ep, NULL, NULL)` is correct — you don't need to specify a local address.

A couple of hard-won warnings from MacSurf's networking work, so you don't relearn them the slow way:

- **Carbon CFM apps cannot do a passive (listening) OT bind.** The OS rejects a caller-chosen listening address categorically — MacSurf verified this across fourteen rounds of attempts before giving up on writing a local server. If your design wants to listen on a port, the platform will fight you; redesign around an outbound client model.
- **Use the `*InContext` flavor of the OT routines.** Open Transport ships two sets: plain (`InitOpenTransport` / `OTOpenEndpoint`) and context-scoped (`InitOpenTransportInContext` / `OTOpenEndpointInContext`). For a Carbon app on OS 9, use the context-scoped ones — initialize an OT client context at startup with `InitOpenTransportInContext(kInitOTForApplicationMask, &ctx)` and open every endpoint through that context. This is what the shipping MacSurf code does. The deeper lesson is the durable one: when OT calls crash at fixed addresses inside `OTClientLib`, the cause is almost always an *uninitialized or wrong client context* (and very often the missing `'carb'` marker, which is what initializes that context in the first place), not the specific OT routine you called.

If you want a TLS layer on top — MacSurf does HTTPS natively — that's a much larger undertaking than the HTTP plumbing; see [Networking & TLS](Networking-and-TLS) for how MacSurf does TLS 1.3 directly on the Mac.

## An emulator-first development loop

You do not want to reboot a G3 for every build. Set up an emulator and make it your fast inner loop, with the understanding that it's a *smoke test*, not the final word.

The right emulator depends on which OS 9 you target. **SheepShaver** emulates a PowerPC Mac and runs Mac OS 7.5.2 through **9.0.4** — and not a version higher, because it doesn't emulate the memory management unit, which 9.1 and 9.2.2 require ([sheepshaver.cebix.net](https://sheepshaver.cebix.net/); [SheepShaver, Wikipedia](https://en.wikipedia.org/wiki/SheepShaver)). It needs a Mac OS install plus a PowerMac ROM image you supply yourself. **QEMU** (`qemu-system-ppc` with the `mac99` machine type) goes all the way to **9.2.2** and sidesteps the ROM-copyright question by using the free OpenBIOS firmware instead of an Apple ROM ([QEMU PowerMac docs](https://www.qemu.org/docs/master/system/ppc/powermac.html); [installing 9.2.2 on QEMU](https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/)). One trap worth flagging up front: do **not** pick Basilisk II — it's a 68K-only emulator and cannot run any PowerPC OS or app at all, which is the most common wrong turn people take here.

MacSurf uses SheepShaver on Linux as exactly this kind of loop: build, launch, confirm Carbon init succeeds, confirm the dependencies resolve, click around. It's genuinely useful for that, and for catching mistakes that slip past a syntax check but break at link or first run.

The part you must take seriously, though: **the emulator is more forgiving than real hardware, and a green light there is not a green light on a G3.** MacSurf has a documented list of bugs that ran perfectly in SheepShaver and crashed on real iron — a live-tracking scroll-bar control definition, a mouse-wheel event class CarbonLib never actually back-ported, control-callback dispatch through the wrong kind of function pointer. The emulator's CarbonLib and Control Manager are simply more tolerant than the silicon. And its networking needs manual configuration, so the emulator is no place to validate real fetch behavior. Use the emulator to move fast; reserve the real machine for the final word, especially on anything hardware-, input-, or network-specific.

## The lessons that generalize

Strip away the browser specifics and a handful of disciplines are what actually keep a classic Mac project shippable. These are the ones MacSurf paid for in debugging time, offered so you don't have to.

**Write disciplined C89, and pick one compiler as your truth.** CodeWarrior 8 is strict C89 and it will reject things your modern instincts produce without thinking: no `//` comments, no `for (int i = ...)` loop-scope declarations, no C99 designated initializers, no compound literals, all variables declared at the top of their block. (`__VA_ARGS__` works as a pre-C99 extension, which is handy for log macros.) None of this is hard once it's a habit, but it's a habit you have to build deliberately, and the way you build it is by compiling *constantly* against the strict compiler rather than discovering a hundred violations at the end. There are also genuine compiler *bugs* to know about — CodeWarrior's PowerPC code generator miscompiles certain `long long` multiplies, for instance — but those are best learned from the dedicated [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page when you hit them.

**Verify on real hardware. Always.** This bears repeating because it's the lesson that costs the most when ignored. The emulator tells you the build is *plausible*. Only a real G3 or G4 tells you it *works*. Every hardware-specific bug in MacSurf's history was invisible in emulation. Budget for a real machine, or for a patient friend who has one.

**Build a file-backed log early.** You will not have a console. You often can't attach a debugger, and when you can (MacsBug on real hardware), it's a heavy instrument that may need an ADB keyboard you don't own. So the most valuable diagnostic MacSurf has is mundane: it writes one line per log call to a plain text file on the Desktop, flushing after *every* write so the file survives an illegal-instruction crash, a frozen machine, or a forced restart. After a crash you open the file and read the last few lines — that's your backtrace. Build this on day one. The discipline that makes it trustworthy: confirm the log actually *fires* (the init call is reached, the file appears, and no feature-macro silently compiled it to a no-op), because a logging channel that's wired but dead is worse than none — it lies to you by omission. The full setup is on [Diagnostics & Debugging](Diagnostics-and-Debugging).

**Respect cooperative scheduling in your architecture, not as an afterthought.** Every design that assumes "I'll just spin a thread for that" has to be unwound on this platform. Decide up front how long work yields and how the event loop stays responsive, because retrofitting cooperation into a blocking design is painful.

And the meta-lesson, the one that's easy to miss: the platform is finished, which is freeing. Nothing here is going to change underneath you. The Toolbox, CarbonLib, Open Transport, the resource model — they are what they were in 2002 and what they'll be in 2052. Every hour you spend learning them compounds, because the knowledge never decays. That's a rare thing in software, and it's a big part of why building for a 25-year-old Mac is more rewarding than it has any right to be.

If you're going further, the [Resources & Prior Art](Resources-and-Prior-Art) page collects the references MacSurf leaned on — NetSurf, Classilla, the ssheven SSH client for Open Transport patterns, the emulator and toolchain archives, and the communities (68kmla and friends) where people who do this still hang out. You are not the first to try, and you won't be on your own.

## Sources

- Macintosh Toolbox overview — https://wiki.retrotechcollection.com/Macintosh_Toolbox
- Carbon (API), Wikipedia — https://en.wikipedia.org/wiki/Carbon_(API)
- Apple, *Carbon Porting Guide* (Legacy, 2002) — https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf
- Apple, cooperative multitasking concepts — https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html
- Mike Ash, *The Mac Toolbox followup* (WaitNextEvent / yielding) — https://www.mikeash.com/pyblog/the-mac-toolbox-followup.html
- Preferred Executable Format (PEF/CFM), Wikipedia — https://en.wikipedia.org/wiki/Preferred_Executable_Format
- CodeWarrior Pro 8 archive (Macintosh Garden) — https://macintoshgarden.org/apps/codewarrior-pro-8x
- CodeWarrior Pro 8 (Macintosh Repository) — https://www.macintoshrepository.org/1351-codewarrior-pro-8-x
- CodeWarrior 8.2 / 8.3 updaters (Macworld) — https://www.macworld.com/article/155573/codewarrior-5.html and https://www.macworld.com/article/156765/codewarrior-6.html
- Retro68 cross-compiler — https://github.com/autc04/Retro68
- Open Transport, Wikipedia — https://en.wikipedia.org/wiki/Open_Transport
- Resource fork, Wikipedia — https://en.wikipedia.org/wiki/Resource_fork
- Creator code, Wikipedia — https://en.wikipedia.org/wiki/Creator_code
- SheepShaver — https://sheepshaver.cebix.net/ and https://en.wikipedia.org/wiki/SheepShaver
- QEMU PowerMac emulation — https://www.qemu.org/docs/master/system/ppc/powermac.html
- Installing Mac OS 9.2.2 on QEMU — https://devonhubner.org/Install_MacOS_9.2.2_on_a_qemu-based_VM/
