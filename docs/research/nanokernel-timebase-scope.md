# NanoKernel / Timebase Scope

## Background

The PowerPC NanoKernel was Apple-internal firmware for the PowerPC transition, completely undocumented for decades. It is now reverse-engineered: Elliot Nunn (CDG5 project) recovered both Gary Davidian's original Power Macintosh NanoKernel and René Vega's Multitasking NanoKernel. Source is publicly available via the CDG5 project.

---

## Phase 1: Timebase Register (mftb / mftbu) — immediate, zero-risk

The PowerPC Time Base is a 64-bit hardware counter (`TBL` SPR 268, `TBU` SPR 269) readable from **user mode** via `mftb`/`mftbu` — no Toolbox call, no interrupt disable, no privilege needed. IBM designed it this way.

### Tick rate
| Machine          | Bus speed  | TB rate     | Resolution  |
|------------------|-----------|-------------|-------------|
| G3 iMac (dev)    | 66 MHz    | 16.5 MHz    | ~60 ns/tick |
| G4 (common)      | 100 MHz   | 25.0 MHz    | ~40 ns/tick |
| G4 (133 MHz bus) | 133 MHz   | 33.25 MHz   | ~30 ns/tick |

Current `Microseconds()` bottoms out at 1 µs and carries Toolbox call overhead. The timebase gives ~16-33x better resolution at zero overhead.

### CW8 inline assembly

```c
/* C89 — declare at top of block */
unsigned long hi, lo, hi2;
do {
    asm { mftbu hi }   /* read TBU first */
    asm { mftb  lo }   /* read TBL       */
    asm { mftbu hi2 }  /* re-read TBU    */
} while (hi != hi2);  /* retry if TBL wrapped during read */
```

CW8 PPC `asm {}` blocks accept named C local variables directly. No register binding syntax needed; the compiler assigns the variable to a GPR.

### Calibration

Empirical: measure TB ticks across one TickCount boundary (16 667 µs) at startup. Avoids needing to query bus speed via Gestalt at runtime:

```c
tc0 = TickCount();
while (TickCount() == tc0) {}   /* wait for boundary */
tb_start = macsurf_tb_read();
tc0 = TickCount();
while (TickCount() == tc0) {}   /* one full tick = 1/60 s */
tb_end = macsurf_tb_read();
g_tb_ticks_per_us = (tb_end.lo - tb_start.lo) / 16667UL;
```

### Files to create / modify

| File | Change |
|------|--------|
| `browser/netsurf/frontends/macos9/macsurf_timebase.h` | **New** — struct + API (skeleton written at fixes477 start) |
| `browser/netsurf/frontends/macos9/macsurf_timebase.c` | **New** — read, calibrate, to_us (add to MacSurf.mcp) |
| `browser/netsurf/frontends/macos9/macsurf_debug_log.c` | Replace `UnsignedWide g_profile_t0` + `Microseconds()` with `macsurf_tb64` + `macsurf_tb_read()` |
| `macTLS/os9/ostls_entropy.c` | Add `mftb`/`mftbu` reads in `OSTLS_CollectEntropy`, `OSTLS_StirTimer`, `OSTLS_InjectEntropy` |

`macsurf_tb_calibrate()` should be called from `macsurf_profile_reset()` on first use (already the natural call site — no new main.c wiring needed).

For entropy, calibration is NOT needed — we fold raw TBL/TBU into the SHA-256 pool purely for jitter, not for timing. Racing reads (no retry loop) are deliberately good there.

### Linux / non-PPC fallback

```c
#ifdef __MWERKS__
    /* PPC inline asm */
#else
    /* fallback: return {0, 0} */
#endif
```

---

## Phase 2: OT Shim via NanoKernel interrupt dispatch — long-term

### Concept
Currently MacSurf polls Open Transport from the cooperative `WaitNextEvent` loop. The NanoKernel handles hardware interrupt dispatch (now readable from the Elliot Nunn reconstruction). A supervisor-mode shim could:

1. Catch OT packet-ready interrupt in supervisor context
2. Set a single flag in a known user-space address
3. Return immediately

MacSurf's user-mode loop then checks the flag instead of polling — zero wasted CPU when idle.

### Safety envelope (required before any bare-metal work)

- **QEMU first**: Elliot Nunn's `classicvirtio` project proves QEMU handles OS 9 driver/interrupt testing. The shim must pass stress testing there before touching real G3/G4 hardware.
- **Minimal blast radius**: The supervisor shim does exactly one thing — write a flag. No Toolbox calls, no allocs, no stack growth. Return immediately.
- **User-mode catcher**: MacSurf stays entirely in user mode. It reads the flag, clears it, and calls `OTRcv`. The shim never touches socket state.

### References
- Elliot Nunn's CDG5 project: NanoKernel source reconstruction
- `classicvirtio`: QEMU-based OS 9 driver testing environment
- OT notifier (`kOTSyncIdleEvent` + `YieldToAnyThread`): current approach to compare against

### NanoKernel interrupt architecture (from actual source)

Cloned at `/home/patrick/Webs/NanoKernel/` (Elliot Nunn's reversal).
CDG5 project at `/home/patrick/Webs/cdg5/`.

Key files for MacSurf's eventual OT shim work:
- `ExternalInts.s`: External interrupt dispatch table. `ExternalInt0-9`
  are aligned 64-byte handlers. Ethernet MACE fires at IPL 3 (ExternalInt0
  comment: `bit 3: Ethernet IRQ`). DMA at IPL 4 (`bit 4: DMA IRQ`).
- `HotInts.s`: Decrementer interrupt — the cooperative scheduler heartbeat.
  `DecrementerIntSys` / `DecrementerIntAlt` manage the two-context model.
- `Defines.s`: `VecTbl` at SPRG3 offset 244 is the interrupt vector table
  the ROM looks up. Patching an entry here redirects a hardware IRQ.
- `SoftInts.s`: Software interrupt dispatch (from SPRG1/2).

To intercept an OT packet-ready IRQ: patch `VecTbl.ExternalInt0` entry
in the running NanoKernel to a shim that sets a user-space flag before
calling the original handler. This requires supervisor-mode access and
QEMU validation first (see classicvirtio).

### Practical alternative: Multiprocessing Services (MP Tasks)

Apple's documented, Carbon-compatible gateway to the NanoKernel's
preemptive scheduler. Available since Mac OS 8.1, no NanoKernel patching
needed:

- `MPCreateTask` / `MPTerminateTask`: create preemptively-scheduled tasks
  (through the NanoKernel's Alt context).
- `MPAllocateAlignedMemory` / `MPSignalSemaphore`: shared-memory + semaphore
  IPC between the MP task and the Blue Task (cooperative UI).
- Open Transport in an MP task: OT 1.3+ supports multi-threaded endpoints;
  use `OTOpenEndpointInContext` from the MP task's context.

**Proposed architecture for MacSurf:**
```
[NanoKernel Scheduler]
  ├── [Blue Task]   UI: WaitNextEvent, QuickDraw, Carbon controls
  └── [MP Task A]   TLS: macTLS handshake + app-data decrypt
      [MP Task B]   DNS: OT resolver (optional, OT already async)
```

The Blue Task and MP tasks share a ring-buffer in common memory. MP task
writes decrypted data + signals a semaphore. Blue Task checks the semaphore
on each null-event pass and pumps the content pipeline.

**Constraint:** OS 9 has no protected memory — a bug in an MP task crashes
the whole machine. Develop and stress-test in QEMU/SheepShaver first.

### Status: **parked** — Phase 1 done (fixes477), Phase 2 needs QEMU scaffolding

---

## Current State (2026-06-24)

- **Phase 1 DONE (fixes477):** `macsurf_timebase.c` written + shipped.
  `performance.now()` now backed by mftb (~60 ns on G3). `Date.now()`
  now uses GetDateTime+Microseconds (~1 µs) via `duk_config.h` override.
- **Still pending from Phase 1:** macsurf_debug_log.c and ostls_entropy.c
  not yet updated to use mftb — next easy wins.
- **Phase 2 (MP Tasks / NanoKernel shim):** scoped above; needs QEMU first.
- Repos cloned: `/home/patrick/Webs/NanoKernel/`, `/home/patrick/Webs/cdg5/`
