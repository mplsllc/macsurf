# Carbon CFM Open Transport passive bind: a 14-round investigation

**Status: blocked at platform layer. No code workaround exists.**

## The result in one sentence

A Carbon CFM application opening Open Transport endpoints via
`OTOpenEndpointInContext` **cannot bind to a caller-chosen
`InetAddress`** — `OTBind` returns `kOTBadAddressErr (-3150)` for any
combination of port, host, qlen, sync vs async, or protocol stack.
Only `OTBind(ep, NULL, NULL)` and `OTBind(ep, { addr=NULL, qlen=N })`
are accepted, and only with OT picking the local address.

## How we got here

macTLS's original design was a **local HTTP proxy app** that classic
OS 9 browsers (Classilla, iCab, MacSurf) would point at as their HTTP
proxy. The proxy would `OTBind` to `127.0.0.1:8765`, accept
connections from local browsers, perform the upstream HTTPS fetch via
BearSSL, and return decrypted HTTP. This architecture required a
working passive `OTBind` on a caller-chosen port.

Stages A through B5 all landed successfully — BearSSL builds under
CW8, validated TLS 1.2 handshake works against `google.com:443`,
decrypted HTTPS GET responses come back byte-for-byte. The outbound
crypto and OT paths are solid. The block is the listener half.

## What we tried (fixes16 through fixes34)

Across 14 hardware iteration rounds we tested every combination
plausibly relevant:

| Dimension | Values tried |
|---|---|
| Port | 8765, 12345, 0 |
| Host | `INADDR_ANY` (0), `127.0.0.1`, real LAN IP (10.42.0.145) |
| qlen | 0 (client), 1 (server) |
| Protocol stack | `"tcp"`, `"tilisten,tcp"` |
| Sync/async | `OTSetSynchronous` + `OTSetBlocking`, `OTSetSynchronous` alone, `OTSetAsynchronous` + notifier callback |
| `req.addr.maxlen` | 0 (per TN1145 prose), `sizeof(InetAddress)` (per Apple HTTP Server `ToNetbuf`) |
| `OTBind` retBind | NULL, populated with buffer |
| Open call | `OTOpenEndpointInContext` (forced; plain `OTOpenEndpoint` not exported by CarbonLib) |

The final bind 2×2 matrix:

|  | `addr = NULL` | `addr = explicit InetAddress` |
|---|---|---|
| `qlen = 0` | OK | -3150 |
| `qlen = 1` | OK at OT-assigned ephemeral port (e.g. 49423) | -3150 |

Every explicit address fails. Every NULL address works. The
endpoint info confirms the listener gets the right type
(`servtype = T_COTS_ORD`). The bytes of the `InetAddress` we hand
OT are byte-perfect — confirmed via hex dump (`00 02 22 3D 00 00 00 00`
for port 8765 + INADDR_ANY, `00 02 30 39 ...` for port 12345).
`OTInetGetInterfaceInfo` confirms the TCP/IP stack is fully
configured (Mac has a real LAN IP, gateway, DNS).

## Research sweep findings

Searched for:

- **Apple Tech Notes**: TN1145 ("Living in a Dynamic TCP/IP
  Environment") shows `DoIncomingBindOT` — Apple's canonical OT TCP
  server bind. Listing 4 was followed letter-for-letter (config
  `"tcp"`, `req.addr.len = sizeof(InetAddress)`, `qlen = 10`, real
  retBind buffer). Still fails -3150 under CarbonLib.

- **Apple sample code**: Extracted `Http_Server.sit` from a preserved
  Apple FTP mirror (sonixwave). `TNetworkAcceptor::EventOpenComplete`
  uses **`OTAsyncOpenEndpoint`** (plain, no `InContext` variant) and
  succeeds at passive bind. The Carbon CFM equivalent
  (`OTOpenEndpointInContext` + `OTAsyncOpenEndpointInContext`) does
  not. **The sample is pre-Carbon and doesn't go through CarbonLib's
  OT wrapper.**

- **Inside Macintosh: Networking with Open Transport** (PDF): the
  result-codes appendix defines `kOTBadAddressErr` for TCP as
  *"the specified protocol address was in an incorrect format or
  contained illegal information. For TCP/IP this means that the
  address does not exist in the specified domain."* The address
  itself is fine; the InContext path appears to apply additional
  validation we can't satisfy.

- **No documented Carbon CFM TCP listener exists in any archive**
  (Apple opensource.apple.com, GitHub mirrors, Macintosh Garden,
  Macintosh Repository, archive.org Wayback). Every classic-Mac TLS
  project we found (Certainly, bbenchoff/MacTLS, Crypto Ancienne,
  antscode/mbedtls-Mac-68k) is client-only or uses GUSI's POSIX
  socket abstraction over MacTCP/OT rather than native OT directly.
  Apple's own HTTP Server sample is pre-Carbon and uses the
  non-InContext API.

The honest assessment: this isn't a code bug we missed. CarbonLib's
OT wrapper appears to genuinely not support caller-chosen passive
binds, even though no Apple documentation says so explicitly. The
fact that no Carbon CFM TCP server has ever been published — in 20+
years — suggests we're not the first to discover this limit.

## What the platform DOES support

| Operation | Status |
|---|---|
| `OTOpenEndpointInContext("tcp")` | works |
| `OTBind(ep, NULL, NULL)` (qlen=0, OT-picked port) | works (Stages B1-B4) |
| `OTConnect(ep, &remote_addr)` outbound | works |
| `OTSnd / OTRcv / OTSndOrderlyDisconnect` | works |
| `OTBind(ep, { addr=NULL, qlen >= 1 })` (passive on OT-picked port) | works (probeC, port 49423) |
| `OTBind(ep, { addr=<explicit InetAddress>, qlen=anything })` | **fails -3150** |

## Implications for macTLS

The original architecture — *"macTLS Proxy listening on a fixed
port; browsers configure HTTP proxy to that port"* — **cannot ship
on Carbon CFM**. There is no caller-chosen port; only OT-picked
ephemeral ports work, and those change between launches.

Two architectural options remain:

### Option A — Library mode (recommended)

Skip the listener entirely. macTLS ships as a **static C library**
that MacSurf (and any other classic Mac browser project) links
directly. The library exposes a single `OSTLS_Fetch(url, ...)` entry
point built on the verified Stage B4 code path. No `OTBind` with
caller-chosen address ever happens.

This is the architecture **docs/mactls-integration-notes.md (Stage
B5)** already describes in detail. The integration mechanics are
already designed and the per-fetch memory footprint is documented
(~50 KB, comfortable in MacSurf's 16 MB Carbon partition).

Pros:
- No listener, no bind, no Carbon CFM passive-bind limitation
- All existing B1-B4 verified code re-used as-is
- Cleaner integration (single function call vs proxy round-trip)
- No port configuration burden on the user

Cons:
- Coupled to MacSurf rather than system-wide. Other browsers
  (Classilla, iCab) won't benefit unless they're separately
  patched to link macTLS. Crypto Ancienne's `carl` (MPW shell
  tool) remains the only existing option for those.

### Option B — OT-picked port + UX exposure

Keep the proxy architecture but accept that the port is whatever
OT assigns at startup. macTLS Proxy starts → `OTBind(NULL, qlen=1)`
→ logs `"listening on port 49423"` → user reads the port from the
window and configures their browser to `127.0.0.1:49423`. Next
launch, the port may be different; user updates browser config.

Pros:
- Preserves the "system-wide proxy" architecture
- Could in theory be patched into Classilla / iCab / etc.

Cons:
- Port changes between launches (DHCP-style instability)
- User UX is clunky — every launch needs a config update
- We have no proof browsers gracefully handle a proxy whose port
  isn't reachable on startup (Classilla, iCab failure modes
  unknown)
- The library extraction work is needed regardless if anything
  else wants to link the engine

**Recommendation: Option A.** It's the simpler architecture, the
B5 integration notes already lay it out, and the verified Stage B4
code is the library API surface in waiting. Option B can be
explored later if there's demand to ship macTLS as a standalone
service for non-MacSurf browsers.

## What to do next (concrete)

1. Extract `OSTLS_Fetch(target_host_port, server_name, request_path, out_response, out_response_cap, out_msg, out_msg_len)` from `os9/ostls_b4_https_get.c`. The B4 probe is essentially this function already; we rename and stabilise its signature.

2. Move the C1 listener code to `os9/archive/` (or delete; commit history preserves it). It's no longer in the build target but the git log is the authoritative reference for "we tried this for 14 rounds and it didn't work."

3. Update `MacTLSTest` to call the new `OSTLS_Fetch` API directly, replacing the C1 stage. MacTLSTest stays the regression harness.

4. Document the library API in `docs/mactls-library-api.md` for MacSurf-side consumers.

5. On the MacSurf side (separate project), implement `macos9_https_fetcher.c` that calls into the library when an HTTPS URL is requested. This is the Stage B5 integration that was already planned.

The macTLS crypto work is done. What remains is a packaging change,
not a new technical milestone.
