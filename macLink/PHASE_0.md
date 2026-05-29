# macLink — Phase 0

**Status:** seed shipped, awaiting CW8 build + hardware test.
**Goal:** prove that a Faceless Background App under CarbonLib can host an Open Transport loopback listener and accept inbound TCP connections cooperatively. No TLS, no proxy logic — just the bones.

If Phase 0 works, the macLink architecture is unblocked. If it doesn't, the bug is at the OS / OT / FBA layer and we'll learn what to do differently before investing in Phases 1–6.

---

## What ships in this commit

| File | LOC | Purpose |
|---|---:|---|
| [`os9/mlink_prefix.h`](os9/mlink_prefix.h) | ~25 | CW8 prefix file: `__MACOS9__`, `NO_IPV6`, `TARGET_API_MAC_CARBON`, MacTypes-first |
| [`os9/mlink_log.h`](os9/mlink_log.h) | ~30 | File-backed log channel API |
| [`os9/mlink_log.c`](os9/mlink_log.c) | ~230 | "macLink Debug.log" on Desktop, CR-terminated lines, flushed every write, restricted printf set (`%d`/`%ld`/`%p`/`%s`/`%%`) |
| [`os9/mlink_listener.h`](os9/mlink_listener.h) | ~50 | Listener API + default port assignments (HTTPS-CONNECT 8765, SMTP 8587, IMAP 8143, POP3 8110, FTP 8121) |
| [`os9/mlink_listener.c`](os9/mlink_listener.c) | ~250 | One-port OT listener with notifier, accept, log-and-drop. Phase 0 closes accepted connections immediately. |
| [`os9/mlink_main.c`](os9/mlink_main.c) | ~80 | FBA `main()`: cooperative event loop, AppleEvent 'quit' handler, listener startup, graceful shutdown |
| **Total Phase 0 code** | ~640 | |

All three .c files pass Retro68 PPC GCC strict-C89 syntax check (`-std=c89 -pedantic -Wall`) clean.

---

## CW8 project setup (manual, per existing convention)

Per the same workflow MacSurf and macTLS use: a new `.mcp` is created on the Mac side by the maintainer. Linux-side automation doesn't touch `.mcp` files. This doc lists the configuration the maintainer needs to apply.

### Project type

**PPC PEF Application**, Carbon. Save as `macLink Proxy.mcp` in `macLink/`.

### Project files (Source group)

```
mlink_log.c          → macLink/os9/
mlink_listener.c     → macLink/os9/
mlink_main.c         → macLink/os9/
```

That's it for Phase 0. macTLS / BearSSL files land in Phase 1.

### Prefix file

`macLink/os9/mlink_prefix.h`

### Access paths (User Paths, non-recursive)

```
{Project}:os9:
```

### Access paths (System Paths)

```
{Compiler}:MacOS Support:Universal:Interfaces:CIncludes:
{Compiler}:MacOS Support:MacHeaders:
{Compiler}:MSL:MSL_C:MSL_Common:Include:
{Compiler}:MSL:MSL_C:MSL_MacOS:Include:
```

### Required libraries (PPC libraries)

```
CarbonLib
OpenTransportAppPPC
OpenTransportLib
MSL C.Carbon.Lib
MSL Runtime.Lib
```

### PPC PEF Settings

- **Output name:** `macLink`
- **Filetype:** `APPL`
- **Creator:** `mLnk` (case-sensitive, all four bytes)
- **Application Heap (preferred):** 8192 KB
- **Application Heap (minimum):** 4096 KB
- **Carbon flags:** "Carbon Application" checked

### Required `'carb'` resource

Same requirement as MacSurf — without `'carb'` 0 in the resource fork, CarbonLib does not load, OT calls crash. Easiest path: copy `MacSurf.rsrc` from the MacSurf project, change the icon family if you want a different icon, change FREF/BNDL to refer to creator `mLnk`. Save as `macLink.rsrc` and include in the project.

For Phase 0 it is acceptable to ship without a custom icon; the carb-only minimum resource fork is sufficient.

### Background-only bit (the FBA bit)

In the `'SIZE'` resource (the same one that sets the heap sizes), set the **"Background-only"** bit. This makes the Finder treat macLink as background-only: no Dock icon (the Dock did not exist on OS 9 but the moral equivalent is "no menu bar takeover, no Application menu entry"), no front-window required.

CW8 exposes this in the PPC PEF Settings panel under "App is Background-Only" or similar — exact label varies by CW8 sub-version. The bit is `kIsBackgroundApplication` / `0x80` in SIZE.

---

## How to test Phase 0 on hardware

1. Build the project in CW8. Should produce `macLink` PEF application.
2. Move `macLink` to `System Folder/Startup Items/` (or just double-click for ad-hoc testing).
3. Watch the Desktop: `macLink Debug.log` should appear within a second of launch with these lines:
   ```
   [mlink] === macLink startup ===
   [mlink] main: macLink starting up
   [mlink] listener: Open Transport initialised
   [mlink] listener: listening on 127.0.0.1:8765
   [mlink] main: entering event loop
   ```
4. From any TCP client on the same Mac — telnet, MacSurf in proxy-config mode, even a Python one-liner from another host on the LAN over Bonjour if you have it — connect to `127.0.0.1:8765`.
5. The log should add:
   ```
   [mlink] listener: ACCEPT port=8765 peer=127.0.0.1:NNNNN
   ```
   …and the client's connection should close immediately (Phase 0 doesn't proxy).
6. Cmd-Q in any front app while macLink is running won't reach macLink (no front window). The clean way to quit is to send it an Apple Event via AppleScript:
   ```applescript
   tell application "macLink" to quit
   ```
   The log should then show:
   ```
   [mlink] ae: quit received
   [mlink] main: shutdown — closing listeners
   [mlink] main: bye
   ```

## Pass criteria

- **Pass:** the four log markers in step 3 appear, the ACCEPT line in step 5 appears, the quit sequence in step 6 appears, no MacsBug invocations.
- **Soft pass:** all log markers appear but the connection accept hangs or the listener never receives notifier events. Indicates an OT notifier-installation issue we can fix.
- **Hard fail:** macLink doesn't launch, crashes at startup, or doesn't write to the log file. Indicates either the FBA pattern doesn't work as we believe under CarbonLib, or the `'carb'` resource is missing, or the OT init path is wrong.

---

## What this proves

If Phase 0 passes:
- FBA can run under CarbonLib (the core architectural assumption)
- OT can listen on loopback under FBA process context
- The notifier + accept pattern works for inbound TCP
- The file-backed log gives us debug visibility same as MacSurf has
- Process Manager keeps the FBA alive across accept cycles
- AppleEvent 'quit' shutdown works (matters for shutdown reliability)

That clears the path for Phase 1 (macTLS server-side termination) and Phase 2 (CA / cert minting) to start in parallel. Phase 3 (real TLS-bridging proxy) lands when both arrive.

---

## What Phase 0 does NOT prove

- No TLS yet — Phase 1 work
- No certificate minting yet — Phase 2 work
- No multi-listener (only 8765 is up) — Phase 4 work
- No Internet Config integration — Phase 4 work
- No Control Panel UI — Phase 5 work
- No STARTTLS — Phase 6 work
- No installer / Setup Wizard — Phase 7 work

If Phase 0 passes cleanly, we are unblocked on **all** of these.

---

## Build environment summary

- **Compiler:** CW8 / strict C89, MacSurf's conventions
- **No `inline`, no `//`, no for-scope decls, no designated initializers**
- **Linux-side syntax check** uses Retro68 PPC GCC: `/home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc -std=c89 -pedantic -Wall -Wno-long-long`
- **Project file structure** under `macLink/os9/` mirrors `macTLS/os9/`
- **Sister projects' files** (`macTLS/os9/ostls_*.c`, `macTLS/bearssl/src/**/*.c`) are not yet linked into macLink — Phase 1 work
