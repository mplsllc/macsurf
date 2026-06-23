# macTLS Integration Handoff

**Target Agent:** MacSurf Integration Agent
**Component:** `macTLS` (Native HTTPS for OS 9)
**Status:** Core library validated on hardware; ready for NetSurf integration.

## Overview

The `macTLS` library has successfully passed standalone hardware validation (`MacTLSTest`) on Mac OS 9. It is now capable of performing asynchronous TLS 1.2 handshakes and streaming HTTP/1.1 chunked responses over Open Transport without blocking the Carbon event loop.

The goal is to wire this library into MacSurf to replace the external `macsurf-proxy` for `https://` URLs.

## Current Capabilities (v0.3 / v1.0)

1.  **Async State Machine (`ostls_async.h/c`)**: Provides a non-blocking API (`OSTLS_New`, `OSTLS_Start`, `OSTLS_Pump`, `OSTLS_Read`, `OSTLS_Write`). It manages Open Transport events and BearSSL's engine seamlessly.
2.  **Production Entropy (`ostls_entropy.h/c`)**: Gathers cryptographically acceptable noise (mouse deltas, microsecond jitter) during the app's idle loop.
3.  **HTTP Convenience Layer (`ostls_http.h/c`)**: Includes a robust `OSTLSChunkDecoder` for handling `Transfer-Encoding: chunked` and request formatters (`OSTLS_HTTP_FormatGet/Post`).

## Integration Plan

To integrate `macTLS` into MacSurf, you need to create a new NetSurf fetcher (`macos9_https_fetcher.c`).

### 1. Build System Updates
Add the following `macTLS` files to the `MacSurf.mcp` CodeWarrior project:
*   `macTLS/os9/ostls_async.c`
*   `macTLS/os9/ostls_entropy.c`
*   `macTLS/os9/ostls_fetch.c` (if blocking fallback is needed)
*   `macTLS/os9/ostls_http.c`
*   `macTLS/os9/ostls_log.c`
*   `macTLS/os9/ostls_time.c`
*   `macTLS/os9/ssl_engine_cw8.c`
*   `macTLS/os9/ostls_b3_anchors.c`

Ensure `macTLS/os9/` and `macTLS/bearssl/inc/` are in the Access Paths.

### 2. The Fetcher Lifecycle (`macos9_https_fetcher.c`)
*   **Setup:** On `fetch_setup`, initialize an `OSTLSConnection` using `OSTLS_New`.
*   **Start:** On `fetch_start`, call `OSTLS_Start`.
*   **Poll:** In the `poll` callback (called by NetSurf's main loop):
    1.  Call `OSTLS_CollectEntropy()` to feed the global pool.
    2.  Call `OSTLS_Pump(conn, 6, &ev)` to advance the state machine.
    3.  If `ev == kOSTLSEventHandshakeDone`, use `OSTLS_HTTP_FormatGet` and `OSTLS_Write` to send the HTTP request.
    4.  If `OSTLS_GetState` is Open/Closing, `OSTLS_Read` decrypted bytes.
    5.  Parse HTTP headers manually. Once headers end (`\r\n\r\n`), feed the body bytes through `OSTLS_HTTP_ChunkDecoderProcess` (if chunked) and dispatch to NetSurf via `FETCH_DATA` callbacks.
*   **Teardown:** On `fetch_abort` or `fetch_free`, call `OSTLS_Close` and `OSTLS_Dispose`.

### 3. Fetcher Registration
In `browser/netsurf/frontends/macos9/macos9_fetcher_init.c`:
*   Register your new native fetcher for `https`.
*   Modify `macos9_http_fetcher_register` so the old proxy-based fetcher *only* registers for `http`.

## Known Constraints & Next Steps

Once the basic integration works, the following issues must be addressed before the proxy can be permanently retired:

1.  **Trust Anchors (CA Bundle):**
    *   *Current State:* `ostls_b3_anchors.c` contains only ~10 hardcoded root CAs. Many sites will fail validation.
    *   *Task:* Implement a way to load a larger Mozilla `cacert.pem` from the Mac OS 9 filesystem, or provide a UI prompt allowing the user to bypass certificate errors (`BR_ERR_X509_NOT_TRUSTED`).
2.  **Session Resumption (Performance):**
    *   *Current State:* Every resource fetch triggers a full asymmetric handshake (1-3 seconds on a G3).
    *   *Task:* Wire BearSSL's session cache (`br_ssl_session_parameters`) into the fetcher so connections to the same host can resume quickly.
3.  **Memory / Concurrency:**
    *   *Current State:* Each `OSTLSConnection` allocates ~32KB for BearSSL buffers.
    *   *Task:* Ensure the `MAX_HTTPS_F` (max concurrent fetchers) is capped (e.g., 4 or 6) to prevent OS 9 heap exhaustion.
4.  **Date/Time:**
    *   *Current State:* Validation will fail (`kOSTLSAsync_ClockBefore2000`) if the Mac's clock isn't set.
    *   *Task:* Ensure this specific error surfaces a clear message to the user.