# macTLS v0.2 — Non-blocking TLS stream API

**Status: design draft. No code yet.**

## Position in the roadmap

```
v0.1.0  blocking OSTLS_Fetch, capped at one fixed buffer.       SHIPPED
v0.2.0  non-blocking TLS stream API; OSTLS_Fetch becomes a      THIS DOC
        thin wrapper. Streaming reads from the start.
v0.3.0  HTTP convenience helpers (POST request body, redirect
        helper, session resumption).
v1.0.0  production entropy. Security claim is real.
```

The architectural call in v0.2 — and the load-bearing decision this
doc is committing to — is that **macTLS stops trying to be an HTTP
library**. It becomes a non-blocking TLS transport. HTTP request
formatting, response parsing, chunked-transfer decoding, redirects,
session lifecycle: all of that lives in the caller. macTLS owns
exactly: TCP via Open Transport, TLS via BearSSL, the bytes between
them, and the validation. Nothing else.

This is the right shape because MacSurf — the only consumer we
care about — already has a full HTTP state machine in
`macos9_http_fetcher.c`. A TLS stream replaces the encrypted half of
that machine cleanly; an async HTTP wrapper would be a second state
machine competing with the first.

## The public API

Two new files:

```
os9/ostls_async.h      — the API
os9/ostls_async.c      — the implementation
```

`ostls_fetch.{h,c}` stays as-is; `OSTLS_Fetch` is retained as a
blocking convenience wrapper, but its implementation is rewritten on
top of the async API as the v0.2 self-test (success criterion #7
below).

```c
#ifndef OSTLS_ASYNC_H
#define OSTLS_ASYNC_H

#include <MacTypes.h>      /* OSErr, UInt16, UInt32, OSStatus */

typedef struct OSTLSConnection OSTLSConnection;  /* opaque */

typedef enum {
    kOSTLSStateIdle = 0,        /* New() called, Start() not yet  */
    kOSTLSStateConnecting,      /* OTConnect in flight            */
    kOSTLSStateHandshaking,     /* BearSSL handshake in progress  */
    kOSTLSStateOpen,            /* app-data flowing both ways     */
    kOSTLSStateClosing,         /* close_notify sent, draining    */
    kOSTLSStateClosed,          /* peer closed; readable until 0  */
    kOSTLSStateFailed           /* terminal failure; see Diag.    */
} OSTLSState;

typedef enum {
    kOSTLSEventNone = 0,        /* Pump returned with nothing to report */
    kOSTLSEventConnected,       /* TCP up; handshake about to begin     */
    kOSTLSEventHandshakeDone,   /* TLS up; app-data flows next          */
    kOSTLSEventReadable,        /* bytes available via OSTLS_Read       */
    kOSTLSEventWritable,        /* OSTLS_Write will accept more bytes   */
    kOSTLSEventClosed,          /* peer initiated close (clean FIN)     */
    kOSTLSEventFailed           /* state advanced to kOSTLSStateFailed  */
} OSTLSEvent;

typedef struct OSTLSConfig {
    const char *host;                  /* "google.com" or "10.0.0.1"        */
    UInt16      port;                  /* 443                                */
    const char *server_name;           /* SNI + cert hostname match          */

    UInt32 connect_timeout_ticks;      /* 0 = use default (30 s at 60 Hz)    */
    UInt32 handshake_timeout_ticks;    /* 0 = use default (30 s at 60 Hz)    */

    void *user_refcon;                 /* opaque to macTLS; for caller use   */
} OSTLSConfig;

typedef struct OSTLSDiagnostics {
    OSTLSState state;
    OSErr      ostls_err;         /* kOSTLSFetch_* style                     */
    OSStatus   ot_err;            /* OT result of the failing OT call        */
    int        br_err;            /* BR_ERR_* from BearSSL, if any           */
    UInt16     cipher_suite;      /* 0 until handshake completes             */
    UInt32     bytes_sent;        /* plaintext sent via OSTLS_Write          */
    UInt32     bytes_received;    /* plaintext returned via OSTLS_Read       */
} OSTLSDiagnostics;

/* --- lifecycle --- */
OSErr OSTLS_New(OSTLSConnection **out_conn, const OSTLSConfig *config);
OSErr OSTLS_Start(OSTLSConnection *conn);
void  OSTLS_Close(OSTLSConnection *conn);   /* TLS close_notify + OT FIN     */
void  OSTLS_Dispose(OSTLSConnection *conn); /* free; may be called without Close */

/* --- advancement --- */
OSErr OSTLS_Pump(OSTLSConnection *conn, UInt32 max_steps, OSTLSEvent *out_event);

/* --- streaming I/O --- */
OSErr OSTLS_Write(OSTLSConnection *conn, const void *buf, UInt32 len, UInt32 *out_written);
OSErr OSTLS_Read (OSTLSConnection *conn,       void *buf, UInt32 cap, UInt32 *out_read);

/* --- introspection --- */
OSTLSState OSTLS_GetState(OSTLSConnection *conn);
void       OSTLS_GetDiagnostics(OSTLSConnection *conn, OSTLSDiagnostics *out_diag);

#endif /* OSTLS_ASYNC_H */
```

Seven functions plus two structs and two enums. That's the entire
v0.2 surface.

## What "one step" means

`OSTLS_Pump(conn, max_steps, *out_event)` is the heart of v0.2. The
contract is:

- Pump runs **at most `max_steps` atomic actions** and returns.
- It never blocks on the network.
- It never sleeps.
- It never calls `WaitNextEvent`.
- The caller decides cadence by how often they call Pump and with
  what `max_steps` value.

One **step** is one of these atomic actions:

1. Drain a notifier-set OT event flag and react (e.g. transition
   from Connecting to Handshaking on `T_CONNECT`).
2. Call `OTRcv` once on the endpoint; deliver any returned bytes to
   BearSSL's recvrec buffer.
3. Call `OTSnd` once with whatever BearSSL has queued in its
   sendrec buffer.
4. Advance BearSSL's engine state machine once
   (`br_ssl_engine_current_state` + one of the matching record-buffer
   pumps).
5. Copy a chunk of decrypted plaintext from BearSSL's recvapp
   buffer into the connection's internal plaintext-read buffer.
6. Copy a chunk of caller-queued plaintext from the connection's
   internal write buffer into BearSSL's sendapp buffer.

`out_event` receives the *most interesting* event observed during
the steps performed — there's a precedence (Failed > Closed >
HandshakeDone > Connected > Readable > Writable > None) so the
caller doesn't have to call Pump multiple times to discover the
state change. If nothing interesting happened, `*out_event =
kOSTLSEventNone` and Pump returns `noErr` — the caller can
`WaitNextEvent` and try again next tick.

**Recommended `max_steps` for a MacSurf-like caller:** 4-8 per
fetcher poll-tick. Enough to make handshake progress but not so much
that one connection monopolises the UI loop while N other fetches
sit idle.

A `max_steps == 0` call is legal and degenerate: just returns the
current state without doing anything (useful for "check if we
failed without advancing").

## State machine

```
                  OSTLS_New
                      │
                      ▼
                    Idle
                      │
                      │  OSTLS_Start (begins OTConnect)
                      ▼
                  Connecting
                      │
                      │  T_CONNECT (notifier) + Pump
                      │  emits kOSTLSEventConnected
                      ▼
                 Handshaking
                      │
                      │  BR_SSL_SENDAPP|RECVAPP visible + Pump
                      │  emits kOSTLSEventHandshakeDone
                      ▼
                    Open ◄───┐
                      │      │ OSTLS_Read / OSTLS_Write
                      │      │ Pump emits Readable / Writable
                      │      └─── (loop)
                      │
        OSTLS_Close   │           peer FIN
        (caller)      │           (T_ORDREL via notifier)
                      ▼                ▼
                  Closing ─────────► Closed
                      │
                      │  no more bytes to drain
                      ▼
                   Closed
                      │
                      │  OSTLS_Dispose
                      ▼
                    (gone)


  ── failure transitions (from any non-terminal state) ──

  Connecting / Handshaking / Open / Closing
       │
       │  OT error, BearSSL error, timeout
       ▼
     Failed
       │
       │  OSTLS_Dispose
       ▼
     (gone)
```

Failed and Closed are both terminal. The difference: Closed means a
clean TLS close_notify exchange completed (or peer-initiated FIN
arrived cleanly). Failed means something went wrong; consult
`OSTLS_GetDiagnostics` for the specific cause.

The `kOSTLSStateResolving` state from the brief is **collapsed into
Connecting**. Open Transport's `OTInitDNSAddress` + `OTConnect`
treats DNS as part of the connect call; the notifier fires
`T_CONNECT` after both DNS and the TCP three-way handshake complete.
Separating them in our state machine would expose plumbing the
caller can't act on.

## Implementation strategy

The async path is essentially Apple's `TNetworkAcceptor` pattern
inverted for client use (which Certainly's `ot_transport.c` is also
an instance of):

1. **OT in async mode.** `OTAsyncOpenEndpointInContext` opens with a
   notifier already attached. All OT calls return immediately; the
   notifier fires at interrupt time for `T_OPENCOMPLETE`, `T_CONNECT`,
   `T_DATA`, `T_ORDREL`, `T_DISCONNECT`, etc.

2. **Notifier is dumb.** It sets volatile flags on the
   `OSTLSConnection` struct and returns. No memory allocation, no
   Toolbox calls, no logging. Pump reads the flags at app time.

3. **BearSSL drives in sync mode** within Pump — the engine state
   machine is single-threaded by Pump's serialisation.

4. **Buffering layer between BearSSL records and caller reads.**
   BearSSL hands us records in 16 KB chunks; the caller may
   `OSTLS_Read` 256 bytes at a time. macTLS keeps a small ring
   buffer (or just a position-tracked linear buffer) so partial
   reads don't lose data. Same on the write side: the caller queues
   plaintext into the connection's write buffer; Pump feeds it into
   BearSSL's sendapp buf as room becomes available.

5. **OTClientContext.** macTLS keeps using
   `OTAsyncOpenEndpointInContext` against the same `g_ostls_ot_context`
   that v0.1 set up. No change to OT initialisation contract.

## Memory ownership

`OSTLS_New` allocates the connection via `NewPtrClear`. Size is
fixed at compile time (roughly 50 KB:
`br_ssl_client_context` + `br_x509_minimal_context` + 33 KB I/O
buffer + the small ring buffers and bookkeeping).

The connection owns the OT endpoint and the BearSSL contexts. The
caller owns the pointer.

**Lifetime contract:**

| Caller action | Connection state | Allowed? |
|---|---|---|
| `OSTLS_New` then `OSTLS_Dispose` (never Start) | Idle → gone | yes |
| `OSTLS_New` then `OSTLS_Dispose` (forget to Close) | any → gone | yes; macTLS aborts the connection internally |
| `OSTLS_New` then `OSTLS_Close` then `OSTLS_Dispose` | any → Closed → gone | yes; preferred |
| `OSTLS_Write` then `OSTLS_Dispose` without draining reads | Open → gone | yes; pending response bytes are discarded |
| Calling `OSTLS_Pump` after `Failed` | Failed | legal; returns immediately with `kOSTLSEventFailed` |
| Calling any function on a `Disposed` connection | gone | UB; caller's responsibility |

Caller is expected to track the pointer; macTLS does not maintain
any global "list of live connections" because that'd add reentrancy
risk in the notifier path.

## Reentrancy

`OSTLS_Pump` is **not reentrant on the same connection** — a
caller must not call Pump from inside a Pump callback. (There are no
callbacks from Pump in v0.2; events are returned via `out_event`.)

`OSTLS_Pump` on **different** connections is reentrant in
principle, but v0.2 doesn't promise it. MacSurf calls one fetcher at
a time per poll-tick in practice; multi-connection concurrency in a
single tick is a v1.x problem.

The notifier callback can fire at any time (interrupt-time) — it
only touches volatile flags and the connection's small inbound
notifier-state struct. App-time code reads those flags; serialisation
between notifier and app-time code is via standard OT volatile-flag
conventions (the same pattern Apple's HTTP Server sample documents
and that Certainly uses).

## Diagnostics surface

`OSTLS_GetDiagnostics(conn, &diag)` returns a snapshot. Fields:

- `state` — current `OSTLSState` (also via `OSTLS_GetState` directly)
- `ostls_err` — the `kOSTLSFetch_*`-style code from v0.1 (reused;
  new namespace would be churn)
- `ot_err` — `OSStatus` of the failing OT call, if any
- `br_err` — `BR_ERR_*` from BearSSL, if the failure is at the TLS
  layer
- `cipher_suite` — populated post-handshake; 0 before
- `bytes_sent` / `bytes_received` — plaintext bytes that have flowed
  through `OSTLS_Write` and `OSTLS_Read`. Useful for progress bars.

Diagnostics is read-only and cheap; safe to call from Pump-driven
loops without consideration.

## `OSTLS_Fetch` as the self-test

In v0.2, `OSTLS_Fetch` (the v0.1 blocking convenience API) gets
rewritten on top of the async API. The new implementation looks
like:

```c
OSErr OSTLS_Fetch(const char *host, UInt16 port,
                  const char *server_name, const char *path,
                  void *out_buf, UInt32 out_cap, UInt32 *out_len,
                  char *out_msg, UInt32 out_msg_len)
{
    OSTLSConnection *conn = NULL;
    OSTLSConfig cfg;
    /* ... populate cfg ... */

    OSErr err = OSTLS_New(&conn, &cfg);
    if (err) goto fail;

    err = OSTLS_Start(conn);
    if (err) goto fail;

    /* Drive Pump until handshake complete or failure. */
    while (OSTLS_GetState(conn) < kOSTLSStateOpen) {
        OSTLSEvent ev;
        OSTLS_Pump(conn, 8, &ev);
        if (ev == kOSTLSEventFailed) { err = ...; goto fail; }
    }

    /* Push the HTTP request once. */
    /* ... format the GET line into a stack buffer ... */
    UInt32 written = 0;
    OSTLS_Write(conn, request_line, request_len, &written);
    /* request_len fits in BearSSL's send-app buffer; this is a
       single Write call. */

    /* Drain bytes until peer closes or out_buf fills. */
    UInt32 collected = 0;
    for (;;) {
        OSTLSEvent ev;
        OSTLS_Pump(conn, 8, &ev);

        if (ev == kOSTLSEventReadable) {
            UInt32 r = 0;
            OSTLS_Read(conn, (char*)out_buf + collected,
                       out_cap - collected, &r);
            collected += r;
            if (collected >= out_cap) break;
        }
        if (ev == kOSTLSEventClosed) break;
        if (ev == kOSTLSEventFailed) { err = ...; goto fail; }
    }

    *out_len = collected;
    /* format out_msg with cipher suite + byte count */

    OSTLS_Close(conn);
    OSTLS_Dispose(conn);
    return kOSTLSFetch_OK;

fail:
    if (conn) OSTLS_Dispose(conn);
    return err;
}
```

If `OSTLS_Fetch` keeps passing the same hardware test it passes at
v0.1 (capture the HTTP 301 from google.com:443), the async API
underneath is correct.

## v0.2 success criteria

v0.2 ships when **all** of these hold:

1. `OSTLSConnection` is opaque to the caller.
2. `OSTLS_New` allocates via `NewPtrClear`; `OSTLS_Dispose` frees
   via `DisposePtr`. No leaked OT endpoints across malformed
   lifecycle sequences.
3. `OSTLS_Start` begins a non-blocking connect.
4. `OSTLS_Pump(conn, 4, ...)` returns within milliseconds, regardless
   of network state, on a healthy connection. Verified with TickCount
   bracketing around the Pump call in the test harness.
5. The caller can call `OSTLS_Write` post-handshake; bytes are
   accepted up to BearSSL's sendapp budget and `out_written` reports
   the actual count.
6. The caller can call `OSTLS_Read` while bytes are available;
   subsequent calls return 0 bytes without error until more arrive.
7. `OSTLS_Fetch` is reimplemented on top of the async API and still
   passes the v0.1 hardware test against google.com:443.
8. MacTLSTest's regression harness gains a Stage D2 that drives the
   async API explicitly (separate from the convenience-wrapper test
   for `OSTLS_Fetch`):

```
Stage D2   async open google.com:443 ...
Stage D2   handshake OK TLS1.2 ECDHE-ECDSA CHACHA20 0xCCA9
Stage D2   wrote 54 request bytes
Stage D2   read chunk 95 bytes
Stage D2   peer closed cleanly
Stage D2   OSTLS_Dispose OK
```

   Both Stage D (blocking) and Stage D2 (async) should land in the
   same run, in sequence.

## What v0.2 deliberately doesn't do

- **No POST request body API.** v0.3.
- **No redirect helper.** v0.3.
- **No session resumption.** Each connection is a fresh handshake.
- **No connection pooling.** v0.3 if there's demand.
- **No HTTPS-aware proxy support.** Whatever you Write, macTLS sends;
  whatever it receives, you Read.
- **No certificate-pinning override.** The 10 embedded anchors are
  the trust set; v0.3 may add API to extend it.
- **Production entropy is still v1.0.** The async refactor doesn't
  change which seed source BearSSL gets.
- **No mid-connection re-handshake.** RFC 5746 renegotiation isn't
  implemented; if a server requests it, the connection closes.

## Open design questions to resolve before coding

1. **Notifier installation order.** Apple's HTTP Server sample
   passes the notifier to `OTAsyncOpenEndpointInContext` directly
   (set at open time). Certainly's `ot_transport.c` calls
   `OTInstallNotifier` separately after open. Both work; the
   open-time variant is one less function to remember to call.
   **Decision:** install at open time.

2. **OT bind for outbound endpoints.** v0.1's `OSTLS_Fetch` uses
   `OTBind(ep, NULL, NULL)` (sync) to bind to an OT-picked local
   port before connect. Async mode also needs this — but Pump has
   to handle the `T_BINDCOMPLETE` event. Add a `kOSTLSStateBinding`
   state? Or fold into Connecting since it's quick and uninteresting?
   **Decision:** fold into Connecting. The caller doesn't care about
   bind vs connect distinction.

3. **`OTRcvOrderlyDisconnect` consumption.** When the peer sends FIN,
   OT delivers `T_ORDREL` via the notifier. We must call
   `OTRcvOrderlyDisconnect` exactly once to consume the event before
   any subsequent OT call. Pump handles this. Caller never sees
   `T_ORDREL` directly; we transition to `kOSTLSStateClosed`.

4. **Plaintext read buffer size.** BearSSL's recvapp_buf can hold up
   to ~16 KB. The caller might Read in much smaller chunks. We need
   an internal buffer for partial reads. Sized at ~4 KB seems right
   — most callers will Read in 1-4 KB chunks anyway.
   **Decision:** 4 KB internal plaintext-read buffer, ring-buffered.

5. **Cancellation during handshake.** If the caller calls
   `OSTLS_Close` (or `OSTLS_Dispose`) mid-handshake, we should abort
   cleanly. BearSSL doesn't have a "cancel handshake" API per se;
   the OT side gets `OTSndDisconnect` (not orderly) to tear down
   the half-finished session.
   **Decision:** Close during handshake = `OTSndDisconnect` + state
   Failed (because the handshake didn't complete; truthful state).

6. **`OSTLS_Pump` re-entry from inside a Pump-driven loop in the
   caller.** Pump never blocks, so a caller's Pump-poll-Pump-poll
   loop is fine; the question is whether Pump-A on connection-A can
   be called while Pump-B is mid-call on connection-B. In v0.2 each
   connection is independent; cross-connection Pump is safe in
   principle. Document as supported but not relied on by the test
   harness in v0.2.

7. **The notifier's interaction with multiple connections.** OT
   installs the notifier per-endpoint, not globally; each
   connection's notifier sees only its own endpoint's events. Safe
   by construction.

## What the API doesn't say but the integrator should know

- `OSTLS_New` does **not** initiate any network activity. It
  allocates and validates. `OSTLS_Start` begins the connect.
- After `OSTLS_Start` succeeds, the caller MUST call Pump
  periodically until the connection reaches `Open` or `Failed`. If
  the caller stops calling Pump mid-handshake, the connection
  effectively stalls; eventually the connect/handshake timeout fires
  and the connection transitions to Failed on the next Pump call.
- `OSTLS_Write` is buffered. A Write that fits in the connection's
  internal write buffer returns immediately even if no bytes have
  actually been transmitted yet. The caller knows the bytes have
  hit the wire when subsequent Pump calls return
  `kOSTLSEventWritable` (meaning the buffer drained and more can be
  accepted).
- `OSTLS_Read` is non-blocking; it returns 0 bytes with `noErr` if
  no data is currently available. Don't busy-loop on Read; drive
  Pump and only Read in response to `kOSTLSEventReadable`.
- The connection's plaintext-read buffer holds up to 4 KB. If the
  caller doesn't Read promptly when Readable fires, BearSSL can't
  decrypt further records (no place to put them) and the TCP
  receive window stops opening. Caller should Read promptly.
- A 60-second handshake-or-IO inactivity timeout fires Pump → Failed.

## Approximate scope

| Component | Size estimate |
|---|---|
| `ostls_async.h` | ~150 lines (API + types) |
| `ostls_async.c` | ~600-800 lines |
| Refactor `ostls_fetch.c` to wrap async API | ~80 lines net (mostly deletion) |
| MacTLSTest Stage D2 wiring | ~100 lines in main.c |

Total: roughly 800-1000 lines of new + ~80 lines net change. Three
weeks of focused work; could compress to two if the Pump event-loop
state machine doesn't fight back during testing.

## Risks

- **Notifier stability on real hardware.** Async OT works in
  principle (Apple's samples, Certainly, ssheven all use it) but
  the interaction with Carbon CFM specifically is less-tested
  ground. Plan: keep Stage D (blocking) live in MacTLSTest as a
  regression baseline so we know if the async path regresses.
- **Pump partial-progress edge cases.** "I made 4 steps and one of
  them was a partial OTSnd; should the event reflect that?" Need a
  cleanly-defined rule. Recommend: the partial-send case
  contributes to `bytes_sent` in diagnostics but doesn't fire a
  Writable event until the buffer has actually drained.
- **Plaintext-buffer-full back-pressure.** If the caller doesn't
  Read fast enough, what happens? Plan: Pump stops calling
  recvapp_ack until there's room in the internal read buffer. TCP
  receive window will eventually close on the peer. That's the
  correct backpressure behavior — same as a regular socket — but
  needs explicit handling in the Pump loop.
- **Lifecycle bugs around Dispose-without-Close.** Easy to leak the
  OT endpoint. Plan: Dispose always tears down OT, regardless of
  state; assert via the test harness that we never leak after
  abnormal disposal.

---

This document is the design checkpoint. Next step: open a fresh
session against macTLS with this as the brief and let an
implementation agent draft `ostls_async.{h,c}` + the Stage D2
wiring + the refactored `OSTLS_Fetch`. Expected output of that
session: a fixes39-fixes45-ish series ending in a green Stage D2
hardware run against google.com:443 with the async API explicitly
exercised.

---

## Amendments after implementation (fixes40..fixes44)

The v0.2 implementation landed across fixes40..fixes43; this
section records where the as-built code diverges from the design
above and why.

**`OSTLSConnection` is monolithic (one heap allocation).**
Internal struct holds the BearSSL contexts, the 33 KB I/O buffer,
4 KB read ring, 4 KB write queue, all OT stable storage (TBind,
TCall, InetAddress, DNSAddress, TEndpointInfo), notifier-set
volatile flags, and string buffers for `host` / `server_name` /
`host_port`. Allocated via `NewPtrClear`. Total resident memory
per connection is roughly 50 KB.

**`OSTLS_Fetch` was not refactored onto the async API.** Brief
listed this as commit step #7 with the caveat "if practical."
Decision: keep v0.1's `ostls_fetch.{h,c}` independent of v0.2 for
the first hardware-verification cycle so any regression in either
path stays distinguishable. If Stage D regresses while Stage D2 is
green, we know the issue is in the blocking implementation, not
the async one (and vice versa). The refactor lands when the two
paths have both been green on hardware for at least one cycle.

**Stage D1 is a separate file (`os9/ostls_d1_probe.{h,c}`)** rather
than living inside `ostls_async.c`. The D1 probe tests just the
async-OT layer (no BearSSL); keeping it isolated makes it valid
prior-art for any future investigation of OT-layer issues.

**Pump precedence as implemented:** Failed > Closed >
HandshakeDone > Connected > Readable > Writable > None. Matches
design doc. `event_priority` helper plus `event_bump` for the
collapse.

**Connect phase substates** (NeedsOpen, OpenInFlight, NeedsBind,
BindInFlight, NeedsConnect, ConnectInFlight, Done) are internal to
the .c file. Publicly, `kOSTLSStateConnecting` covers the whole
arc. The substate lets `pump_connect_step` know what notifier
event to wait for next without exposing plumbing through the
public state enum.

**Pre-Start clock check** moved to `OSTLS_Start`. Brief had it
inside `pump_connect_step`; doing it at Start time is cheaper (one
syscall up front) and surfaces `kOSTLSAsync_ClockBefore2000` from
the call that actually represents "start the connection," which
is more discoverable for the caller.

Result codes namespace bumped to 2000..2017 (was unspecified in
the brief). Disjoint from v0.1's `kOSTLSFetch_*` (1000..1014) and
the C1 / D1 probe namespaces (700..709, 800..812).

The MacTLSTest harness gained Stage D1 (after B3) and Stage D2
(after D). Stage D stays for the blocking regression baseline.

Pending: hardware verification on G3 / OS 9.1. Until both Stage D
and Stage D2 are green in the same run, v0.2 is "code complete,
not validated." Tag bump to v0.2.0 holds until then.
