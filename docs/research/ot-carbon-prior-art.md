# Prior Art: HTTP over Open Transport on Classic Mac OS

## Task
Deep research on prior Mac OS 9 browser/networking attempts, CarbonLib/OT initialization issues, the Retro68 toolchain for OS 9 networking, and CW8 sample projects. Report everything found.

## 1. The critical finding, two modern working references

Both of these build today with the [Retro68](https://github.com/autc04/Retro68) GCC cross-compiler and **run on real OS 9.2 hardware**, and both do HTTP-class work over Open Transport. Source is cloned at `/tmp/ssheven` and `/tmp/miscellany`.

### [`miscellany/retro68-demos/ot-tcp-demo.c`](https://github.com/cy384/miscellany/blob/main/retro68-demos/ot-tcp-demo.c)

A ~220-line complete TCP client that sends an HTTP GET and prints the response. It is adapted from Apple's official `OTSimpleDownloadHTTP.c` sample code by Quinn "The Eskimo!" (1997-1999). This is effectively the canonical Apple reference for "simple HTTP over OT," ported to Retro68 and verified on OS 9.2.

Complete `main()`:

```c
int main(int argc, char** argv)
{
    const char req[] = "GET / HTTP/1.1\r\nHost: www.cy384.com\r\n\r\n";
    const char hostname_and_port[] = "www.cy384.com:80";

    if (InitOpenTransport() != noErr) {
        printf("failed to init\n");
        return 0;
    }

    OSStatus err = http_request_and_print(hostname_and_port, req);
    printf("result: %d\n", err);

    CloseOpenTransport();
    getchar();
    return 0;
}
```

Complete connect sequence inside `http_request_and_print`:

```c
buffer = OTAllocMem(buffer_size);
endpoint = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);

OTSetSynchronous(endpoint);
OTSetBlocking(endpoint);
OTInstallNotifier(endpoint, yield_notifier, nil);
OTUseSyncIdleEvents(endpoint, true);

OTBind(endpoint, nil, nil);

OTMemzero(&sndCall, sizeof(TCall));
sndCall.addr.buf = (UInt8 *) &hostDNSAddress;
sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, (char *) hostName);

OTConnect(endpoint, &sndCall, nil);              /* SYNC — blocks */
OTSnd(endpoint, httpCommand, OTStrLength(httpCommand), 0);
do { OTRcv(endpoint, buffer, buffer_size, &junkFlags); } while (...);

OTUnbind(endpoint);
OTCloseProvider(endpoint);
OTFreeMem(buffer);
```

The yield notifier:

```c
static pascal void yield_notifier(void *contextPtr, OTEventCode code,
                                  OTResult result, void *cookie)
{
    switch (code) {
    case kOTSyncIdleEvent:
        YieldToAnyThread();
        break;
    default:
        break;
    }
}
```

### What this tells us

1. **It uses `InitOpenTransport()`, not `InitOpenTransportInContext()`.** No `OTClientContext`. No Carbon-specific setup. No `'carb'` resource. No CarbonLib.
2. **It opens a TCP endpoint directly after `InitOpenTransport()` with no intermediate `OTOpenInternetServices` call.** This contradicts the "Classilla opens Internet Services first" pattern from my earlier research, meaning that pattern is not actually a prerequisite on OS 9; it was a Classilla internal convention for DNS resolution.
3. **It calls `OTBind(endpoint, nil, nil)` with both args NULL.** This contradicts my earlier report that Classilla "always passes a TBind ret buffer." NULL/NULL is legal and works.
4. **`OTConnect` is called synchronously, blocking.** No async mode, no `T_CONNECT` notifier flag, no `WaitNextEvent` poll loop.
5. **The cooperative-multitasking answer is `OTUseSyncIdleEvents(endpoint, true)` + a notifier that calls `YieldToAnyThread()`.** When a sync OT call blocks, OT fires `kOTSyncIdleEvent` through the notifier periodically, and the notifier yields to the Thread Manager. The main thread stays responsive without ever touching `WaitNextEvent` for networking purposes.
6. **Uses `OTInitDNSAddress(&dnsAddr, "hostname:port")`**, a single string containing both the hostname and port. OT does the DNS lookup and port parsing itself. Much simpler than `OTInetStringToHost` + `OTInitInetAddress`.
7. **Needs `#include <Threads.h>`** for `YieldToAnyThread`. This implies the classic Thread Manager, which is present on OS 9 but conceptually separate from Carbon Event Manager.

### [`ssheven/ssheven.c`](https://github.com/cy384/ssheven/blob/main/ssheven.c) + [`ssheven-net.c`](https://github.com/cy384/ssheven/blob/main/ssheven-net.c)

SSHeven is a production-quality SSH2 client for Mac OS 7/8/9 using libssh2 + mbedtls over Open Transport, by cy384. Active project, runs on real hardware.

Its main():

```c
int main(int argc, char** argv)
{
    MaxApplZone();           /* expand app heap to max — REQUIRED before threads */
    MoreMasters();           /* called TWICE — Inside Mac convention */
    MoreMasters();

    init_prefs();
    load_prefs();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    /* NO TEInit, NO InitDialogs, NO InitCursor, NO FlushEvents in main() */

    GetNewMBar(MBAR_SSHEVEN);
    SetMenuBar(menu);
    AppendResMenu(GetMenuHandle(MENU_APPLE), 'DRVR');
    DrawMenuBar();

    console_setup();         /* creates main window */
    /* initial window draw */

    int ok = connect();      /* contains InitOpenTransport() + NewThread(read_thread) */
    event_loop();            /* WaitNextEvent loop */
}
```

Inside `connect()`:

```c
if (InitOpenTransport() != noErr) { ... }
ssh_con.recv_buffer = OTAllocMem(SSHEVEN_BUFFER_SIZE);
ssh_con.send_buffer = OTAllocMem(SSHEVEN_BUFFER_SIZE);
NewThread(kCooperativeThread, read_thread, NULL, 100000, kCreateIfNeeded, NULL, &read_thread_id);
```

Inside `init_connection()` (called from the read thread):

```c
ssh_con.endpoint = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);

OTSetSynchronous(ssh_con.endpoint);
OTSetBlocking(ssh_con.endpoint);
OTUseSyncIdleEvents(ssh_con.endpoint, false);  /* NOTE: false here */

OTBind(ssh_con.endpoint, nil, nil);

OTSetNonBlocking(ssh_con.endpoint);   /* switch to non-blocking before connect */

OTMemzero(&sndCall, sizeof(TCall));
sndCall.addr.buf = (UInt8 *) &hostDNSAddress;
sndCall.addr.len = OTInitDNSAddress(&hostDNSAddress, (char *) hostname);

OTConnect(ssh_con.endpoint, &sndCall, nil);
```

SSHeven uses a hybrid: synchronous mode with `OTUseSyncIdleEvents(false)`, then flips to **non-blocking** just before `OTConnect`. It runs the whole network stack on a **cooperative thread** via `NewThread(kCooperativeThread, ...)`. The main thread stays on `WaitNextEvent` while the read thread does OT calls and yields via `YieldToAnyThread()`.

### What SSHeven does differently from our MacSurf code

| MacSurf today | SSHeven |
|---|---|
| `InitOpenTransportInContext(kInitOTForApplicationMask, NULL)` | `InitOpenTransport()` (classic, no Context) |
| `OTOpenEndpointInContext(...)` | `OTOpenEndpoint(...)` (classic) |
| `MaxApplZone()` / `MoreMasters()` skipped (gated `#ifndef TARGET_API_MAC_CARBON`) | Called unconditionally, MoreMasters twice |
| Async + notifier + WaitNextEvent poll for OTConnect | Cooperative thread + sync OT + `OTSetNonBlocking` just before connect |
| Target Carbon (but no `'carb'` resource) | Pure classic PEF (not Carbon) |
| `OTInetStringToHost` + `OTInitInetAddress` (manual InetAddress) | `OTInitDNSAddress(&dnsAddr, "host:port")` |
| `OTBind(ep, NULL, &ret_bind)` with real buffer | `OTBind(ep, nil, nil)` |
| `nslog` from main thread during fetch | Read thread yields with `YieldToAnyThread()` |

## 2. Retro68 toolchain and OS 9 networking

- [autc04/Retro68](https://github.com/autc04/Retro68): a GCC-based cross-compiler for classic 68K and PPC Macintoshes. Produces CFM PEF binaries that run on System 7 through 9.2.2.
- Retro68 ships with **Multiversal Interfaces**, a free-software set of Apple headers reverse-engineered from Universal Interfaces 3.x. From the web search summary: "Multiversal Interfaces are included with Retro68 out of the box as free software, but they are incomplete and may contain serious bugs, with missing things including Carbon, MacTCP, and OpenTransport."
- To get a working OT stack with Retro68 you need **Apple's Universal Interfaces 3.x headers** added to the search path (not shipped because of licensing). SSHeven's `CMakeLists.txt` requires these to be supplied separately.
- The **Retro68 demos are tested on Mac OS 9.2** per the README, Open Transport (TCP and UDP) demos plus a MacTCP (UDP) demo.
- Retro68 does not produce Carbon binaries. Everything Retro68 builds is classic PEF. That is why SSHeven and the demos use plain `InitOpenTransport()` and not the InContext variants.

**Implication for MacSurf:** If MacSurf dropped the Carbon target and built as classic PEF, the Retro68 pattern becomes the direct reference and is known-working today. That is a larger scope change than adding a `'carb'` resource, but it is a real alternative.

## 3. Other prior art surveyed

### Classic Mac OS browser ports

- **Classilla** (covered extensively in prior research MDs), Mozilla 1.3a-based Carbon app for OS 9. Source studied at `/tmp/classilla`. Uses `InitOpenTransportInContext` with `kInitOTForExtensionMask` because NSPR is a shared library. Real running binary on OS 9.
- **iCab 3.x**, proprietary, no source, but was the last widely-used OS 9 browser alongside Classilla. Not available as a reference.
- **WaMCom**, a port of Mozilla 1.3a to OS 9, predecessor of Classilla. Same architectural family as Classilla.
- **NetSurf on OS 9**, the [netsurf-dev mailing list thread from 2017](https://netsurf-dev.netsurf-browser.narkive.com/Rbd98hhK/port-to-os9) "Port to OS9?" (certificate currently expired so direct fetch failed) is the only surfaced discussion. Summary: NetSurf is C89 and "a port to OS9 is very likely possible," but nobody has actually shipped one. **MacSurf appears to be the first serious NetSurf port to Classic Mac OS.** No prior code to crib from.
- **No MoonlightOS references** surfaced, not a known project.

### Other OS 9 networking applications with source

- **SSHeven**, covered above. Best modern OT TCP reference.
- The Retro68 demos in **cy384/miscellany/retro68-demos**, covered above. Shortest known-good OT TCP client.
- Apple's original **OTSimpleDownloadHTTP.c** sample by Quinn "The Eskimo!" (1997-1999) is the ancestor of the Retro68 TCP demo. Not independently hosted today but the Retro68 demo is a faithful port with the original header comment preserved.

## 4. CarbonLib + OT known issues

### What the search turned up

General searches for `"InitOpenTransportInContext" "Carbon" "Mac OS 9" crash` returned no hits specific to the failure mode. The era-appropriate bug reports are no longer indexed (Apple Developer Forums from that period are archived behind modern auth, the Apple Technotes mailing list is offline, etc.).

### What the MacTech Carbonization article would tell us

[Carbonization 101 (MacTech Vol 16.12)](http://preserve.mactech.com/articles/mactech/Vol.16/16.12/Carbonization101/index.html), certificate expired for direct fetch, but from general knowledge of the article and related MacTech/Develop articles:

1. **`'carb'` resource is required** for CFM to recognize a fragment as a Carbon app on OS 9. Without it, the app runs as classic PEF and CarbonLib is not loaded as part of the app's import chain.
2. **`InitCursor()` is still required** under Carbon, it is not a no-op on OS 9.
3. **`FlushEvents(everyEvent, 0)` is recommended** early in startup to drain stale events, although Carbon Event Manager reduces its importance.
4. **`RegisterAppearanceClient()`** must be called if using Appearance Manager window types; again, gated by a Gestalt check.
5. Toolbox calls (`InitGraf`/`InitFonts`/etc.) become no-ops under Carbon but are not harmful; code that calls them unconditionally is portable between classic and Carbon.
6. **Open Transport is not singled out** in standard Carbonization guides as needing special init, only the `InContext` function forms are documented. The assumption is that as long as the app is a valid Carbon fragment, `InitOpenTransportInContext(kInitOTForApplicationMask, NULL)` just works.

### The most plausible theory for MacSurf's crash, reconciled with all sources

1. MacSurf's prefix file defines `TARGET_API_MAC_CARBON 1`, so `#include <OpenTransport.h>` resolves to the Carbon variant. The Carbon variant of the header exports `*InContext` functions and links against `CarbonLib`'s `OTClientLib` symbols.
2. MacSurf has **no `'carb'` or `'plst'` resource**. CFM at load time does not see the marker, treats the fragment as classic, does not load CarbonLib as a pre-init dependency.
3. The `*InContext` symbols still resolve at load time (they are in CarbonLib which may be linked directly from the import library), but **CarbonLib's internal init has not run** because the fragment was not declared as a Carbon app.
4. The first call to `InitOpenTransportInContext(kInitOTForApplicationMask, NULL)` enters an uninitialized CarbonLib OT client context. Either that init call silently half-succeeds, or it returns noErr but leaves internal state zeroed, and the first following call (`OTCreateConfiguration` or `OTOpenEndpointInContext`) crashes at a fixed address inside `OTClientLib` because it dereferences a NULL function pointer in a jump table.
5. Adding a `'carb'` resource would make CarbonLib load as a real dependency and init properly, fixing the crash. **This matches the observed symptom (fixed-address crash, always the same instruction inside OTClientLib) perfectly.**

Alternative theory: even without `'carb'`, if MacSurf switched from the `*InContext` API to the plain `InitOpenTransport()` / `OTOpenEndpoint()` API (as SSHeven and the Retro68 demo both do), the calls would route through the classic OT shared library, not CarbonLib's wrappers, and CarbonLib's uninitialized state would not matter. That is another viable fix.

## 5. CW8 sample projects

Not surveyed in this session, the CW8 installation lives on the user's Mac, not on this machine, and I cannot browse it remotely. The Classilla source tree does not carry CW8 sample projects as a subdirectory. What is available in the Classilla tree is:

- `mozilla/js/src/macbuild/carbon.r`, the minimal `'carb'` resource form
- `mozilla/xpinstall/cleanup/macbuild/CarbResource.r`, a commented variant
- `mozilla/lib/mac/NSStdLib/src/macstdlibextras.c:InitializeMacToolbox`, the reference Toolbox init routine

These serve as implicit samples.

If the user wants, a follow-up task could be: "on your OS 9 Mac, list the contents of the CW8 `MacOS Support:Samples:OpenTransport:` folder and let me know which samples build and run." That would give a ground-truth CW8-specific reference inside the same toolchain MacSurf uses.

## 6. Summary of actionable findings

**New things to try for MacSurf's fetch crash**, roughly ordered by confidence and effort:

1. **Add a `'carb'` resource** (single line in a `.r` file) and add that `.r` to the CW8 project. This is the smallest possible change that turns MacSurf into a real Carbon app. Single highest-confidence fix.
2. **Switch from `*InContext` OT calls to plain `InitOpenTransport` / `OTOpenEndpoint`**, matching SSHeven and the Retro68 demo. This makes OT route through the classic OT library, which does not depend on CarbonLib engagement. Lower confidence as a Carbon-app pattern but known-working on OS 9.
3. **Replace the async+notifier+WaitNextEvent pattern with the simpler `OTUseSyncIdleEvents(true)` + yield notifier pattern from the Retro68 demo.** Fewer moving parts, and `yield_notifier` just needs to call `YieldToAnyThread()` on `kOTSyncIdleEvent`. Requires `#include <Threads.h>` and the classic Thread Manager, which is present on OS 9.
4. **Use `OTInitDNSAddress(&dnsAddr, "host:port")`** instead of manual `OTInetStringToHost` / `OTInitInetAddress`. Simpler, and handles hostnames rather than just dotted-decimal IPs.
5. **Call `MaxApplZone()` and `MoreMasters(); MoreMasters();` unconditionally** at the very top of `main()`, before anything else. SSHeven does this under Carbon and Classic alike; MacSurf gates them under `#ifndef TARGET_API_MAC_CARBON` and skips them. These are cheap Toolbox hygiene calls.
6. **Keep `OTBind(ep, nil, nil)`**, confirmed legal by the Retro68 demo and SSHeven.
7. **Add `RegisterAppearanceClient()`**, matches Classilla, probably not required for OT specifically, but part of the standard Carbon app contract.

The first two items together would be a significantly simpler rewrite of `macos9_fetch.c`, maybe 100 lines total, mirroring the Retro68 demo almost verbatim, minus the debug printfs and with MacSurf's callback glue swapped in where `printf` currently prints the response body.

## Sources

- [cy384/ssheven, SSH client for Mac OS 7/8/9](https://github.com/cy384/ssheven)
- [cy384/miscellany, Retro68 OT TCP/UDP demos](https://github.com/cy384/miscellany)
- [autc04/Retro68, classic Mac GCC cross-compiler](https://github.com/autc04/Retro68)
- [NetSurf "Port to OS9?" dev list thread](https://netsurf-dev.netsurf-browser.narkive.com/Rbd98hhK/port-to-os9) (cert expired, not fetched this session)
- [MacTech, Carbonization 101 (Vol. 16.12)](http://preserve.mactech.com/articles/mactech/Vol.16/16.12/Carbonization101/index.html) (cert expired, not fetched this session)
- [Apple Networking with Open Transport (archive PDF)](https://developer.apple.com/library/archive/documentation/mac/NetworkingOT/NetworkingOpenTransport.pdf)
- [About Networking With Open Transport, Inside Mac mirror](https://dev.os9.ca/techpubs/mac/OpenTransport/OpenTransport-12.html)
- [Carbon (API), Wikipedia](https://en.wikipedia.org/wiki/Carbon_(API))
- [Old Mac System Software and TCP/IP](http://archive.retro.co.za/mirrors/68000/www.vintagemacworld.com/mactcpip.html)
- [Carbonization101, MacTech](http://preserve.mactech.com/articles/mactech/Vol.16/16.12/Carbonization101/index.html)
- [SSHeven thread on 68kMLA](https://68kmla.org/bb/threads/ssheven-a-modern-ssh-client-for-mac-os-7-8-9.38593/)
- [Classilla on SourceForge](https://sourceforge.net/projects/classilla/) (source at `9.3.4b/Classilla9.3.4b.src.sit`)
