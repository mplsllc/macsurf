# macTLS library API

Two parallel public surfaces:

- **v0.1 — blocking convenience.** `OSTLS_Fetch(...)`. Synchronous;
  the call returns after the full handshake + GET + response capture
  is done (or fails). Header: `os9/ostls_fetch.h`. Implementation:
  `os9/ostls_fetch.c`. Verified end-to-end on G3 / OS 9.1.

- **v0.2 — non-blocking TLS stream.** `OSTLS_New / Start / Pump /
  Write / Read / Close / Dispose` plus introspection getters.
  Socket-like; the caller drives a state machine via Pump and never
  freezes their UI event loop. Header: `os9/ostls_async.h`.
  Implementation: `os9/ostls_async.c`. Code complete; hardware
  verification pending.

The v0.2 API is the intended long-term surface for host-app
integration (MacSurf and beyond). v0.1's `OSTLS_Fetch` is kept as
the regression baseline and for callers who want a simple one-shot
blocking call.

## Purpose

Perform validated HTTPS connections from a classic Mac OS 9 /
PowerPC application without freezing the UI. v0.1 returns
decrypted response bytes from a single GET; v0.2 exposes the
underlying TLS stream so callers own the HTTP semantics (request
shape, redirects, chunked decoding, POST).

## Supported TLS path

- TLS 1.2 only (BearSSL full profile)
- Cipher suites the engine will negotiate, in order of preference:
  - `0xCCA9` TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
  - `0xCCA8` TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
  - `0xC02B/0xC02C` ECDHE-ECDSA AES-GCM
  - `0xC02F/0xC030` ECDHE-RSA AES-GCM
  - (others available via `br_ssl_client_init_full`)
- ChaCha20-Poly1305 is preferred over AES-GCM on PowerPC where AES
  has no hardware acceleration. Verified on G3 / OS 9.1: cipher
  suite `0xCCA9` against google.com.
- 10 embedded root CAs cover Amazon, DigiCert (G2/G3), Google (GTS
  R1/R2/R3/R4), Let's Encrypt (ISRG X1/X2), and Starfield Services
  G2. See `os9/ostls_b3_anchors.h` for the full list with expiry
  dates; rotation script at `tools/regenerate_anchors.sh`.

## v0.1 — `OSTLS_Fetch`

```c
#include "ostls_fetch.h"

OSErr OSTLS_Fetch(
    const char *host,           /* "google.com" or "10.42.0.145"       */
    UInt16      port,           /* 443                                  */
    const char *server_name,    /* SNI + cert hostname match            */
    const char *path,           /* "/" or "/api/v1/posts"               */
    void       *out_buf,        /* caller buffer for response bytes     */
    UInt32      out_cap,        /* size of out_buf                      */
    UInt32     *out_len,        /* actual bytes copied (optional)       */
    char       *out_msg,        /* status string (always NUL-terminated)*/
    UInt32      out_msg_len     /* size of out_msg (recommend >= 180)   */
);
```

Returns `kOSTLSFetch_OK` (0) on success or a `kOSTLSFetch_*` code
in the 1000..1019 range. `out_msg` always carries a short diagnostic
string including the underlying BearSSL or OT error code where
applicable.

## v0.2 — async stream API

```c
#include "ostls_async.h"

typedef struct OSTLSConnection OSTLSConnection;

typedef enum {
    kOSTLSStateIdle, kOSTLSStateConnecting, kOSTLSStateHandshaking,
    kOSTLSStateOpen, kOSTLSStateClosing, kOSTLSStateClosed,
    kOSTLSStateFailed
} OSTLSState;

typedef enum {
    kOSTLSEventNone, kOSTLSEventConnected, kOSTLSEventHandshakeDone,
    kOSTLSEventReadable, kOSTLSEventWritable,
    kOSTLSEventClosed, kOSTLSEventFailed
} OSTLSEvent;

OSErr OSTLS_New(OSTLSConnection **out_conn, const OSTLSConfig *config);
OSErr OSTLS_Start(OSTLSConnection *conn);
OSErr OSTLS_Pump(OSTLSConnection *conn, UInt32 max_steps, OSTLSEvent *out_event);

OSErr OSTLS_Write(OSTLSConnection *conn, const void *buf, UInt32 len, UInt32 *out_written);
OSErr OSTLS_Read (OSTLSConnection *conn,       void *buf, UInt32 cap, UInt32 *out_read);

void  OSTLS_Close  (OSTLSConnection *conn);
void  OSTLS_Dispose(OSTLSConnection *conn);

OSTLSState OSTLS_GetState(OSTLSConnection *conn);
OSErr      OSTLS_GetLastError(OSTLSConnection *conn);
UInt16     OSTLS_GetCipherSuite(OSTLSConnection *conn);
void       OSTLS_GetDiagnostics(OSTLSConnection *conn, OSTLSDiagnostics *out_diag);
void *     OSTLS_GetUserRefcon(OSTLSConnection *conn);
```

Result codes 2000..2017 in the `kOSTLSAsync_*` namespace, disjoint
from `kOSTLSFetch_*` (1000..1014).

Caller drives the connection via `OSTLS_Pump(conn, max_steps, &ev)`:
each Pump call performs at most `max_steps` atomic actions (one
OT operation, one BearSSL advance, or one buffer copy) and returns
the most-interesting event observed. Recommended `max_steps` for a
typical host with a `WaitNextEvent` loop is 4-8 per fetcher poll
tick.

Multiple `OSTLS_Read` calls per Pump are allowed; drain until
`out_read == 0 && err == noErr`, then go back to Pump. `OSTLS_Write`
buffers into an internal 4 KB queue; partial writes are normal when
the buffer is full and the caller should retry the remainder after
`kOSTLSEventWritable` fires.

See [`docs/mactls-v0.2-async-design.md`](mactls-v0.2-async-design.md)
for the full design rationale (state machine diagram, Pump step
definition, lifecycle table, reentrancy contract).

## Caller responsibilities

- **Open Transport must be initialised before the first call** to
  `OSTLS_Fetch`:
  ```c
  OTClientContextPtr g_ostls_ot_context = NULL;
  InitOpenTransportInContext(kInitOTForApplicationMask,
                             &g_ostls_ot_context);
  ```
  The library expects the global `g_ostls_ot_context` to be in
  scope and valid.
- Close OT at shutdown:
  ```c
  CloseOpenTransportInContext(g_ostls_ot_context);
  ```
- The caller allocates `out_buf` and `out_msg`. The library never
  allocates on the caller's behalf and does not own the buffers.
- The caller must not invoke `OSTLS_Fetch` reentrantly from inside
  another `OSTLS_Fetch` (v1 uses static BearSSL contexts).

## Memory ownership

| Pointer | Owner | Lifetime |
|---|---|---|
| `host` / `server_name` / `path` | caller | only needs to be valid during the call |
| `out_buf` | caller | must be valid during the call; library writes ≤ out_cap bytes |
| `out_msg` | caller | must be valid during the call; library writes a NUL-terminated string ≤ out_msg_len |
| `out_len` (if non-NULL) | caller | written before return |

BearSSL contexts (`br_ssl_client_context`, `br_x509_minimal_context`)
and the bidirectional I/O buffer (`BR_SSL_BUFSIZE_BIDI` = 33 178 bytes)
live in macTLS's BSS — caller doesn't see them. Per-fetch resident
memory total is ~50 KB, comfortable within MacSurf's 16 MB Carbon
partition.

## Blocking behaviour

- **Synchronous and blocking.** The call returns when the entire
  handshake + HTTP exchange completes, fails, or hits the 60-second
  deadline.
- The host app's `WaitNextEvent` loop will not be serviced during
  the call. For a UI app this manifests as a frozen window during
  the fetch.
- v2 will add an async / callback-based variant for use from inside
  a cooperative event loop. v1 is deliberately synchronous to keep
  the first ship narrow.

## Error code ranges

```
1000  kOSTLSFetch_BadArgs           — NULL pointer, zero port, etc.
1001  kOSTLSFetch_ClockBefore2000   — Mac clock unset / PRAM battery dead
1002  kOSTLSFetch_OTConfigFail      — OTCreateConfiguration("tcp") failed
1003  kOSTLSFetch_OTOpenEndptFail   — OTOpenEndpointInContext failed
1004  kOSTLSFetch_OTBindFail        — OTBind (NULL,NULL) outbound failed
1005  kOSTLSFetch_OTDnsAddrFail     — OTInitDNSAddress failed
1006  kOSTLSFetch_OTConnectFail     — TCP connect to host:port failed
1007  kOSTLSFetch_EntropyFail       — entropy injection rejected
1008  kOSTLSFetch_ClientResetFail   — br_ssl_client_reset returned 0
1009  kOSTLSFetch_OTSndFail         — OTSnd error mid-transfer
1010  kOSTLSFetch_OTRcvFail         — OTRcv error mid-transfer
1011  kOSTLSFetch_HandshakeTimeout  — 60s deadline expired
1012  kOSTLSFetch_BearSSLError      — handshake or record-layer failure
                                       (BR_ERR_X509_*, BR_ERR_SSL_*,
                                       etc. in out_msg)
1013  kOSTLSFetch_RequestTooBig     — GET line doesn't fit in sendapp buf
1014  kOSTLSFetch_NoBytesReceived   — peer closed before any plaintext
```

The Stage A-B probe codes (100..619) are intentionally distinct so
caller-side switch statements can mix library and harness codes
without collision.

## Current limitations (v1)

```
synchronous / blocking fetch
single request per call
HTTP/1.0 + Connection: close only
no streaming callback yet (entire response must fit in out_buf)
no POST / PUT / DELETE yet
no redirect following yet
no chunked transfer-encoding decoder
fixed embedded trust anchor set (full Mozilla set, 121 roots; see ostls_b3_anchors.h)
no root-store UI (anchors are baked in at build time)
no session resumption — every call does a full handshake
```

Entropy is **no longer a gap**: macEntropy v1.0 (hardware-validated on
a real G3, 2026-05-29) replaced the original Stage-A stub with a
SHA-256 accumulator fed by clock/mouse/stack/OT-packet-arrival jitter
and a Preferences-folder seed file persisted across boots, feeding
BearSSL's engine HMAC-DRBG. See [MACENTROPY_SCOPE.md](../MACENTROPY_SCOPE.md)
and `os9/ostls_entropy.c`.

## Example usage

```c
#include <OpenTransport.h>
#include "ostls_fetch.h"

OTClientContextPtr g_ostls_ot_context = NULL;

int main(void)
{
    OSErr err;
    UInt32 len = 0;
    unsigned char response[256];
    char msg[180];

    InitOpenTransportInContext(kInitOTForApplicationMask,
                               &g_ostls_ot_context);

    err = OSTLS_Fetch(
        "google.com",       /* host        */
        443,                /* port        */
        "google.com",       /* server_name */
        "/",                /* path        */
        response, sizeof response,
        &len,
        msg, sizeof msg);

    if (err == kOSTLSFetch_OK) {
        /* response[] holds 'len' bytes of decrypted HTTP, e.g.
         *   "HTTP/1.0 301 Moved Permanently\r\nLocation: ...\r\n"
         */
    } else {
        /* msg holds a diagnostic string */
    }

    CloseOpenTransportInContext(g_ostls_ot_context);
    return 0;
}
```

For the integration path into MacSurf specifically, see
[mactls-integration-notes.md](mactls-integration-notes.md).

## v0.3 roadmap (not yet shipped)

- Redirect handling + chunked transfer-encoding decoder built into
  the library (currently both belong to the caller's HTTP layer).
- POST / PUT request body support.
- TLS session resumption -- skip the full handshake on repeated
  connections to the same host within the same session.
- Connection pooling -- reuse the OT endpoint across fetches.

## v1.0 (security-ready) roadmap

- ~~Production entropy gathering.~~ **Done — macEntropy v1.0**
  (hardware-validated on G3, 2026-05-29): SHA-256 accumulator fed by
  clock / mouse / stack / OT-packet-arrival jitter + a persisted
  Preferences-folder seed file, feeding BearSSL's engine HMAC-DRBG.
  See `os9/ostls_entropy.c` and [MACENTROPY_SCOPE.md](../MACENTROPY_SCOPE.md).
  The remaining v1.0 item is the host stir seam (`OSTLS_StirEntropy`),
  which lands with the MacSurf fold-in so a real event loop feeds it.
