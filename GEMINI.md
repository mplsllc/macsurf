# GEMINI.md - Foundational Mandates for MacSurf (v1.5)

This file contains the foundational mandates, technical constraints, and operational workflows for the MacSurf project. **These instructions take absolute precedence over general agent workflows and tool defaults.**

## 1. Project Identity & Architecture
- **MacSurf:** A lightweight web browser for Classic Mac OS 9 PowerPC (G3/G4) built on a NetSurf engine fork.
- **macTLS:** Native TLS 1.3 stack built on BearSSL, enabling direct HTTPS on OS 9.
- **MacSurf Proxy:** RETIRED. The Go proxy is no longer on the path and should not be reintroduced.
- **Goal:** Native Carbon app running cooperatively on OS 9, delivering real websites without overwhelming vintage hardware.

## 2. Strict Technical Constraints (CodeWarrior 8 / Mac OS 9)
- **Compiler Mode:** Strict C89 only.
  - NO `inline` keywords.
  - NO `//` comments (except inside URLs in block comments).
  - NO C99 designated initializers (`.field = value`).
  - NO `for(int i...)` scope declarations.
  - NO casting to union types (e.g., `(union_type)0`).
  - NO variadic macros (`__VA_ARGS__`).
- **PowerPC Architecture:**
  - Big-endian.
  - **CRITICAL BUG:** CodeWarrior 8 PPC miscompiles `long long` / `int64_t` multiply-by-constant. Any 64-bit fixed-point math must be routed through `double` via `#ifdef __MWERKS__` (PPC has hardware FPU, 52-bit mantissa covers int32 intermediates). Pure int32 multiplies/divides are fine.
- **Threading:** NO preemptive threads. Classic Mac OS is cooperative. You MUST use a `WaitNextEvent` loop.
- **Carbon API:**
  - Use `TARGET_API_MAC_CARBON 1`.
  - Skip `InitGraf`/`InitFonts`/`InitWindows`/etc., but KEEP `InitCursor()` and `FlushEvents`.
  - Call `RegisterAppearanceClient()` at startup.
  - **CRITICAL BUG:** DO NOT use `kWindowStandardHandlerAttribute` in `CreateNewWindow` (causes blank windows).
  - **CRITICAL BUG:** DO NOT cast function pointers to UPPs for CarbonLib (e.g., `NewControlActionUPP(proc)` macro override is unsafe and crashes). Use `TrackControl(ctrl, pt, NULL)`.
  - **CRITICAL BUG:** DO NOT register Carbon event classes that are not available in CarbonLib (e.g., `kEventMouseWheelMoved` crashes the dispatcher).
- **Networking (Open Transport):**
  - The current browser code uses `InitOpenTransportInContext` plus `OTOpenEndpointInContext` with a shared `macos9_ot_context`.
  - Use `OTUseSyncIdleEvents(ep, true)` with a notifier that calls `YieldToAnyThread()` on `kOTSyncIdleEvent`.
  - **HTTP FETCH:** Always send `Accept-Encoding: identity` to prevent servers from sending compressed content the Mac can't decode, which leads to "download instead of load" behavior.
- **JavaScript:**
  - Engine: Duktape 2.7.0 (ES5) + **on-device ES6-to-ES5 transpiler** (supports let/const, arrow functions, template literals, async/await, for-of).
  - Config: `DUK_USE_BYTEORDER=3`, `DUK_USE_PACKED_TVAL`, `DUK_USE_ALIGN_BY=8`, `DUK_USE_NATIVE_CALL_RECLIMIT=128`.
- **Memory & Stack (v1.5):**
  - Preferred partition should be ~195 MB (min ~164 MB) to accommodate modern JS/DOM/CSS.
  - Stack requirement is increased; ensure `MSL_PPC_stack` is sufficient in the prefix.
- **Time & Performance:**
  - **CRITICAL:** The monotonic clock must be functional. A dead monotonic clock breaks reflow throttling and causes UI unresponsiveness.

## 3. macTLS Mandates
- **SHA-384 Self-Test:** macTLS MUST perform a SHA-384 known-answer self-test at startup to verify crypto integrity.
- **Watchdog:** Use wire-progress credit in the TLS watchdog to prevent timeouts during large or slow handshakes.

## 4. Workflow & Deployment Protocol

### Updating Code
1.  **Do not modify `MacSurf.mcp`:** If you create a new `.c` file, state its name clearly so the user can add it to the CodeWarrior project manually.
2.  **Line Endings:** All `.c`, `.h`, `.r` files sent to the Mac must have Mac CR line endings (if sent individually, but standard zip transfer usually suffices if the user decodes properly).
3.  **Pre-flight Check:** Run Linux C89 syntax checks before assuming code is valid for CW8:
    `gcc -fsyntax-only -std=c89 -pedantic -Dinline= -Ibrowser/netsurf/frontends/macos9/shims -Ibrowser/netsurf/frontends -Ibrowser/netsurf/include -Ibrowser/netsurf -include stdbool.h <file.c>`

### Sending Updates to the User
1.  **Monotonic Versioning:** Ask the user for the next fix number (e.g., `fixes415`).
2.  **Selective Archiving:** Create a tar file containing ONLY the modified files or files needing a resync, preserving the full tree structure:
    `tar -cvf fixes415.tar browser/netsurf/content/handlers/html/modified_file.c`
3.  **Deployment via SCP:** Send the tar to the Mac environment:
    `scp -P 2222 -i ~/.ssh/macsurf_push -o StrictHostKeyChecking=no fixes415.tar patrick@localhost:/home/patrick/afp_share/fixes415.tar`

## 5. Debugging & Instrumentation
- **Diagnostic Logging:** `macsurf_debug_log_init()` MUST be called in `main()` at startup.
- **MS_LOG Mandate:** Always include `macsurf_debug.h` in any file using `MS_LOG` to ensure it is treated as a macro and not an implicit external function.
- **SysBeep:** Avoid leaving `SysBeep()` calls in release code.
- **Regression Audits:** Any new subsystem requires its init function to be called in `main.c`, a smoke test on the Mac, and documentation in `CLAUDE.md`.

## 6. Maintenance
- Always keep this file (`GEMINI.md`) and `CLAUDE.md` up-to-date with new architectural shifts, blockers, or critical CW8/OS 9 bugs discovered.
