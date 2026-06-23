# Networking & TLS

This page is for anyone who wants to understand how MacSurf gets bytes off the wire on a machine that predates HTTPS as we know it. We'll go bottom-up: the classic TCP/IP stack (Open Transport), the trick that keeps the UI alive while the network blocks, the HTTP/1.1 layer on top, and then the part that surprises people — a 233 MHz G3 from 1998 doing TLS 1.3 by itself, with no helper machine anywhere in the loop.

That last point matters enough to say up front: **MacSurf talks to origin servers directly.** It opens its own TCP connection, runs its own TLS handshake, validates the certificate against a built-in root store, and reads back the plaintext. There is no proxy. If you've read older notes that mention one, those describe a path that's been gone since mid-2026.

For the bigger picture of where networking sits in the whole system, see the [Architecture Overview](Architecture-Overview). This page zooms into the networking subsystem specifically.

## Open Transport: the classic TCP/IP stack

Mac OS 9 doesn't have Berkeley sockets. The networking API is **Open Transport** — Apple's STREAMS-based stack, introduced in 1995 to replace the older MacTCP, and built into the OS from Mac OS 8.0 onward. It speaks both TCP/IP and AppleTalk through one API and loads protocol modules on demand.[^ot] If you've only ever written `socket()`/`connect()`/`recv()`, the shape is unfamiliar but the concepts map cleanly: an *endpoint* is roughly a socket, you bind it, connect it, send and receive on it, and close it.

MacSurf opens its endpoints through Open Transport's **`*InContext` variants** — `InitOpenTransportInContext`, `OTOpenEndpointInContext`, and friends. The "context" here is a client-context handle that Carbon apps are expected to thread through every OT call. This isn't optional polish. A Carbon application that calls the plain (non-`InContext`) Open Transport functions on OS 9 ends up running through an uninitialized CarbonLib client context and crashes at a fixed address inside the OT client library. So MacSurf initializes one context once, at startup, and hands it to every endpoint it opens.

The initialization happens exactly once in `main()`:

```c
InitOpenTransportInContext(kInitOTForApplicationMask, &macos9_ot_context);
```

and every fetch opens its endpoint against that shared context:

```c
ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, macos9_ot_context);
```

One wrinkle for builders: CodeWarrior 8's Open Transport headers don't define `kInitOTForApplicationMask`. MacSurf defines it manually as `0x00000002`. This is a recurring theme on this platform — the headers that shipped with the compiler are a snapshot in time, and you fill in the constants the docs describe but the headers forgot. (More of that lives on the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page.)

Address setup uses `OTInitDNSAddress`, which takes a single `"host:port"` string and lets Open Transport resolve the name and parse the port for you — simpler than the older two-step `OTInetStringToHost` + `OTInitInetAddress` dance. Outbound TCP clients can bind with `OTBind(ep, NULL, NULL)`; there's no need to request a specific local address, and on Carbon CFM you couldn't get a caller-chosen one anyway (a hard platform limit worth knowing if you ever try to write a *listening* socket on OS 9 — see [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project)).

[^ot]: Open Transport background: https://en.wikipedia.org/wiki/Open_Transport

## Staying alive while the network blocks

Here's the problem that shapes everything about how MacSurf does networking. Mac OS 9 is **cooperatively multitasked** — there are no preemptive threads. Every running program shares the CPU by voluntarily handing it back, and the way you hand it back is by calling `WaitNextEvent` in your main loop. If a program stops yielding, the *entire machine* freezes, not just that one app.[^coop]

Now consider a network read. The Mac sends an HTTP request and waits for the response. On a TLS connection over a slow link that wait can be seconds. A naive blocking `OTRcv` would sit there spinning, never returning to the event loop, and the whole machine would lock up with the spinning-watch cursor until the bytes arrived. That's unacceptable for a browser.

MacSurf solves this with Open Transport's *sync idle events*. When you call `OTUseSyncIdleEvents(ep, true)` on a synchronous, blocking endpoint, Open Transport agrees to periodically fire a `kOTSyncIdleEvent` to your notifier callback while it's stuck waiting on the network. The notifier's whole job is to give time back to the system:

```c
static pascal void yield_notifier(void *ctx, OTEventCode code,
        OTResult result, void *cookie)
{
    if (code == kOTSyncIdleEvent)
        YieldToAnyThread();
}
```

`YieldToAnyThread()` (from the classic Thread Manager — you must `#include <Threads.h>`) cedes the processor cooperatively. The net effect: the fetch code reads as a straightforward synchronous call, but underneath, every idle moment of the network wait is donated back so the cursor animates, other apps breathe, and MacSurf's own event loop keeps turning. The blocking call *looks* blocking to the code that wrote it, while the machine stays responsive. That's the cooperative-multitasking answer to "how do you do synchronous networking without a thread to put it on," and it's the model the whole fetch layer is built on. The setup per endpoint is `OTSetSynchronous` + `OTSetBlocking` + `OTInstallNotifier` + `OTUseSyncIdleEvents(ep, true)`.

There is deliberately no separate `fetch_poll()` tick. Instead, while any fetcher is active, MacSurf shortens its event-loop sleep to a single tick so NetSurf's internal fetcher ring keeps advancing on every pass. Idle, it sleeps longer to be a good cooperative citizen.

[^coop]: Cooperative multitasking on Classic Mac OS — a long compute that never yields freezes the whole machine: https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html

## HTTP/1.1 on top

With a working byte pipe, the HTTP layer is comparatively ordinary — but it's a real HTTP/1.1 client, not a toy. It handles:

- **Chunked transfer encoding** — responses that arrive in length-prefixed chunks rather than with an up-front `Content-Length`, decoded as they stream in.
- **Keep-alive and connection pooling** — once a connection to an origin is open, it's kept and reused for the next request to the same host rather than torn down and rebuilt. On a page with dozens of stylesheets and images, that's the difference between thirty handshakes and one.
- **3xx redirect following** — any `3xx` response carrying a `Location` header is followed to its target.

The pool is sized for the hardware: **128 fetcher slots total, with 16 concurrent HTTP fetches and 16 concurrent HTTPS fetches** running at once, plus a **15-second no-progress timeout** so a stalled connection releases its slot instead of wedging the page forever. Bodies are written to a persistent on-disk cache shared between the HTTP and HTTPS paths, so a revisit can serve from disk instead of re-fetching.

These are conscious budget numbers. A modern page might want to open forty connections in parallel; a G3 with a 16 MB application partition can't afford that much state at once, so the fetcher governs concurrency down to what the machine can carry. When a page wants more parallelism than there are free slots, the extra fetches queue. (Pushing the HTTPS half of the pool higher is on the load-time quality-of-life list — see [docs/status.md](https://github.com/mplsllc/macsurf/blob/master/docs/status.md) for the current queue.)

> **TODO (verify):** HTTPS *cache-hit serving* is intentionally disabled in the current tree pending a synthetic-header rework — the cache still *stores* bodies, but reloads currently re-fetch over a fresh TLS connection rather than serving the stored copy. Confirm this is still the state at the version you're reading; it's the kind of thing that flips back on between releases.

## macTLS: native TLS, no proxy

This is the part with no real precedent. Doing TLS on OS 9 means doing public-key cryptography, a full handshake state machine, certificate-chain validation, and an authenticated record layer — all on a CPU with no AES instructions, no hardware random-number generator, and a C compiler that predates most of the C standard you'd reach for. The conventional workaround in the retro-Mac world has been to offload TLS to a helper that strips it down to plain HTTP. MacSurf doesn't do that. It does TLS itself, on the Mac, direct to the origin server. The component that makes that possible is **macTLS**.

macTLS is a small layer of integration glue sitting on two foundations: **[BearSSL](https://www.bearssl.org/)** (Thomas Pornin, MIT-licensed) for the cryptographic primitives, and **Open Transport** for the sockets. BearSSL is a good fit for this hardware — it's a constant-time, no-malloc-required, small-footprint C library, and it provides exactly the pieces a handshake needs: the bignum arithmetic, the elliptic-curve math, the AEAD ciphers, the hashes, and a minimal X.509 validator. Per-connection memory is around 50 KB (one client context, one X.509 context, a 33 KB I/O buffer), which fits inside the 16 MB Carbon partition with room to spare.

### TLS 1.3, hand-written

BearSSL ships TLS 1.2 and, by design, will never ship TLS 1.3. So macTLS's 1.3 support is a **hand-written TLS 1.3 implementation per [RFC 8446](https://datatracker.ietf.org/doc/html/rfc8446)** — the handshake state machine, the HKDF-based key schedule, and the record layer are all written from scratch, using BearSSL purely for primitives (X25519, the AEADs, HKDF/HMAC/SHA-256, the X.509 validator). The 1.3 code was adapted from **[Certainly](https://github.com/minorbug/Certainly)** by minorbug (MIT), which targeted the Retro68/C99 toolchain; the macTLS port re-engineered it to CodeWarrior 8's strict C89 dialect and wired it to Open Transport, the entropy source, and the trust bundle. (The port also fixed a bug along the way: Certainly's ChaCha20-Poly1305 decrypt skipped the authentication-tag check, so macTLS added a constant-time tag compare.)

On the wire, the cryptography is genuinely current:

- **Key exchange:** X25519 by default, with **multi-curve ECDHE** — X25519 (`0x001D`), NIST P-256 (`0x0017`), and NIST P-384 (`0x0018`) all offered in `supported_groups`. The `key_share` carries an X25519 public key; if a server's policy excludes X25519 (FIPS-only zones, some strict nginx and Cloudflare configs), it answers with a **HelloRetryRequest** naming the curve it wants, and macTLS resends the ClientHello once with a fresh key share on that curve. Single retry, clean state transition, no recursion.
- **Cipher suites:** `TLS_CHACHA20_POLY1305_SHA256` (`0x1303`) and `TLS_AES_128_GCM_SHA256` (`0x1301`). ChaCha20 is the right default here — it's fast in software on a CPU with no AES hardware, where AES would otherwise be a soft-implementation bottleneck.
- **Transcript hash:** SHA-256.

The 1.3 handshake, key schedule, and record layer pass the **RFC 8446 / RFC 8448 test vectors** both on a host machine and on-device.

### Transparent 1.2 fallback

Not every server speaks 1.3. macTLS handles this without bothering the code above it. Every outbound connection opens with a **TLS 1.3 ClientHello that also advertises 1.2 cipher suites** in the legacy field, and the server decides:

- Server picks **1.3** → the hand-written 1.3 path runs the connection.
- Server picks **1.2** → macTLS reconnects and lets BearSSL's own engine drive a complete 1.2 handshake (negotiating something like `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`, suite `0xCCA9`) against the same root store.

The switch is invisible to the caller. The fetcher just reads and writes plaintext through the same async API (`OSTLS_New` / `OSTLS_Start` / `OSTLS_Pump` / `OSTLS_Write` / `OSTLS_Read` / `OSTLS_Close`), and everything below those calls — 1.3 or 1.2 — is hidden. That async, never-blocking `OSTLS_Pump` shape is what fits the cooperative `WaitNextEvent` loop: each pump does a bounded amount of work and returns, so the same UI-stays-alive discipline that governs HTTP governs the TLS handshake too.

### The trust bundle

Authentication verifies the server's certificate chain against the **full Mozilla CA bundle — 121 trust anchors** (the CCADB set, the same roots that ship in mainstream browsers: ISRG, DigiCert, GTS, Sectigo, GlobalSign, Entrust, and the rest). The bundle is baked into the binary at build time, so there's no separate cert store to install or keep updated on the Mac. Hostname and expiry are checked against the Mac's own clock — which means a Mac with a badly-wrong date will reject valid certificates, a real-world failure mode worth remembering when a fresh OS 9 install with a dead PRAM battery can't reach any HTTPS site.

### macEntropy: where the randomness comes from

A TLS library is only as trustworthy as its random numbers, and this is the hardest part to get right on hardware with no RNG. The honest history: early macTLS shipped a placeholder seed — a rotate-XOR derived from the tick count — and said so loudly in its own docs. Every handshake from a given launch shared a predictable seed line. **macEntropy v1.0** closed that hole.

macEntropy keeps a running **SHA-256 accumulator pool**. Everything jittery on the machine gets folded into it: the high-resolution clock (`Microseconds()` + `TickCount()` on every pool access), stack noise, the arrival timing of every network packet during a fetch (`OTRcv` jitter is genuinely unpredictable and needs no cooperation from anything), and — the richest source on a single-user desktop — live mouse and keystroke timing pulled from the event loop on every `WaitNextEvent` pass. A **seed file in the Preferences folder** carries entropy across reboots so the very first handshake after a cold boot isn't drawing from a thin pool. That file is never trusted alone; it's only ever mixed in alongside live samples, and it's written with a different domain-separation tag than the seed handed to TLS, so reading the file doesn't reveal handshake randomness.

When TLS needs a seed, macEntropy clones the pool, mixes a domain-separation tag, finalizes 32 bytes, and folds the result back into the live pool so successive extractions are independent (the output is never the raw running hash). That 32-byte seed feeds **BearSSL's HMAC-DRBG**, which is the actual generator. In short: macEntropy supplies the entropy, BearSSL runs the standard NIST construction on top of it.

The proof that this is real and not just a hash chain that *looks* random regardless of input is the built-in self-test, which extracts a batch of seeds, checks for degenerate output (successive seeds differ, byte values spread, bit balance near 50%), and emits a 32-bit fingerprint. The fingerprint **must differ across separate launches** — and on a G3, across four separate boots, it did (`94A7251B`, `52AB2050`, `665DF442`, `165814CB`). Distinct per-run fingerprints are the signal that per-run entropy is genuinely present.

### What it isn't

So the cryptographic stack on the wire is, end to end:

```
HTTP/1.1
  TLS 1.3  (ChaCha20-Poly1305 or AES-128-GCM, X25519 / P-256 / P-384, SHA-256)
    TCP    (Open Transport)
      IPv4
        Ethernet  (built into a 1998 iMac G3)
```

For honest accounting, here's what macTLS deliberately leaves out of this first modern cut: **TLS 1.3 session resumption** (PSK / session tickets — the abbreviated-handshake win on revisits; 1.2 resumption does work), **client certificates** (server authentication only), **0-RTT early data** (extra attack surface, deferred), and **post-quantum key agreement** (out of scope). These are tracked, not forgotten.

Stated plainly: as far as anyone working on it can find, macTLS is the **first native TLS 1.3 implementation on Classic Mac OS**, verified on real G3 hardware against third-party endpoints (Akamai's TLS 1.3 reporter, BrowserLeaks, How's My SSL, and Cloudflare's trace all confirm a 1.3 negotiation from the actual browser). It shipped in MacSurf v1.3 on 2026-05-29. That's a real result on 25-year-old hardware — and it's also still a young, single-author crypto stack riding on a battle-tested primitives library, not a drop-in replacement for OpenSSL. Treat it as what it is.

## How this connects to the rest of MacSurf

The fetcher is the front door of [The Rendering Pipeline](The-Rendering-Pipeline): `FETCH_HEADER` / `FETCH_DATA` / `FETCH_FINISHED` callbacks feed NetSurf's content cache, which feeds the HTML parser, the CSS cascade, layout, and finally the QuickDraw plotters. The same cooperative-yield discipline described here — never block the event loop, always give time back — runs through the entire frontend; it's the single most important constraint the platform imposes, and it shows up again in how scrolling, redraw, and JavaScript timers are scheduled (see [The JavaScript Engine](The-JavaScript-Engine)).

If you're thinking about reusing the networking approach in your own project, the OT-context setup, the sync-idle-yield notifier, and the "library, not a proxy" decision are all generalizable — that material lives on [Start Your Own Classic Mac Project](Start-Your-Own-Classic-Mac-Project), and curated references (Classilla's OT socket layer, the ssheven SSH client, BearSSL, Certainly) are collected on [Resources & Prior Art](Resources-and-Prior-Art).

## Sources

- Open Transport (Apple's classic TCP/IP stack): https://en.wikipedia.org/wiki/Open_Transport
- Cooperative multitasking on Classic Mac OS: https://developer.apple.com/library/archive/documentation/Carbon/Conceptual/Multitasking_MultiproServ/02concepts/concepts.html
- TLS 1.3, RFC 8446: https://datatracker.ietf.org/doc/html/rfc8446
- TLS 1.3 test vectors, RFC 8448: https://datatracker.ietf.org/doc/html/rfc8448
- BearSSL (cryptographic primitives): https://www.bearssl.org/
- Certainly (the 1.3 handshake macTLS adapted): https://github.com/minorbug/Certainly
- macTLS repository: https://github.com/mplsllc/macTLS
