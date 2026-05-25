# MacSurf Safety Report

Date: 2026-05-21

## Scope

Inspection only. No code changes were made.

Reviewed areas:

- `browser/netsurf/frontends/macos9/`
- `macTLS/os9/`
- `proxy/`
- top-level scripts and tracked docs related to deployment/secrets

Third-party/vendor trees under `browser/` and `macTLS/bearssl/` were not exhaustively audited line-by-line unless they were directly wrapped or invoked by first-party code.

## Executive Summary

The codebase does not appear to contain hardcoded API keys, private keys, or other obvious embedded secrets in tracked first-party source files.

There are, however, several serious security issues:

1. The proxy is effectively an unrestricted forward proxy / CONNECT tunnel.
2. The proxy can be memory-exhausted by large upstream responses.
3. The Mac OS 9 HTTP fetcher has stack-buffer overflow paths from long URLs.
4. The Mac OS 9 HTTP header parser reads past allocated buffers.
5. `macTLS` still uses a deliberately insecure entropy stub in real TLS setup paths.

There are also privacy and operational hygiene issues around logging and credential handling.

## Findings

### 1. High: Open proxy / SSRF / unrestricted CONNECT tunneling

Evidence:

- [proxy/proxy.go](/home/patrick/Webs/macsurf/proxy/proxy.go:38)
- [proxy/proxy.go](/home/patrick/Webs/macsurf/proxy/proxy.go:90)
- [proxy/README.md](/home/patrick/Webs/macsurf/proxy/README.md:15)

Details:

- `handleHTTP` forwards any absolute URL whose host is present.
- `handleConnect` dials `r.Host` directly with no destination allowlist, port restrictions, or RFC1918 / localhost blocking.
- Authentication is optional, and the README shows unauthenticated startup as the default path.

Impact:

- If exposed beyond a trusted LAN, the service can be abused as an open proxy.
- It can be used for SSRF into internal services, metadata endpoints, loopback services, or arbitrary TCP targets via `CONNECT`.

Recommendation:

- Require authentication by default.
- Add destination policy: block loopback, link-local, RFC1918, and non-approved ports unless explicitly allowed.
- Decide whether `CONNECT` is even needed for the product model; if not, remove it.

### 2. High: Proxy buffers entire upstream bodies with no hard cap

Evidence:

- [proxy/proxy.go](/home/patrick/Webs/macsurf/proxy/proxy.go:60)
- [proxy/proxy.go](/home/patrick/Webs/macsurf/proxy/proxy.go:70)

Details:

- `io.ReadAll(resp.Body)` reads the full response into memory before replying.
- There is no maximum body size, streaming threshold, or per-request memory budget.

Impact:

- A large or intentionally unbounded upstream response can exhaust RAM and kill the proxy.
- This is remotely triggerable through any reachable origin.

Recommendation:

- Add a hard response-size cap.
- Reject or stream oversized responses safely.
- Apply separate limits for redirects, headers, and body size.

### 3. High: Stack-buffer overflow risk in Mac OS 9 HTTP fetcher request construction

Evidence:

- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:391)
- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:423)
- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:437)

Details:

- `req[2048]` is filled with `sprintf(...)` using the full URL in proxy mode.
- `path_buf[1024]` is also built with `sprintf(...)` from path and query components.
- The code computes a `total` length in the path/query branch but does not actually use it to bound the `sprintf`.

Impact:

- A long URL or long path/query can overwrite stack memory in the browser process.
- This is user-triggerable through pasted URLs and may also be triggerable via redirects or page-generated URLs depending on call path.

Recommendation:

- Replace `sprintf` with bounded formatting.
- Reject oversized URLs early.
- Keep explicit size accounting all the way through request construction.

### 4. High: Header parser uses string functions on non-NUL-terminated network buffers

Evidence:

- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:578)
- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:728)

Details:

- Received header bytes are appended with `memcpy`.
- The buffer length is tracked in `h_len`, but no trailing NUL byte is appended before `strstr(c->h_buf, "\r\n\r\n")`.
- `mfs_parse_headers` also begins with `strstr(c->h_buf, "\r\n\r\n")`.

Impact:

- `strstr` and later `strlen` can read beyond the allocated buffer, causing undefined behavior.
- A malicious or malformed server response can crash the browser and may expose memory contents during debugging.

Recommendation:

- Treat headers as length-delimited data, not C strings.
- If string APIs are retained, over-allocate by one byte and maintain a terminator after every append.

### 5. High: `macTLS` uses a deliberately insecure entropy stub in real TLS paths

Evidence:

- [macTLS/os9/ostls_entropy.c](/home/patrick/Webs/macsurf/macTLS/os9/ostls_entropy.c:2)
- [macTLS/os9/ostls_entropy.c](/home/patrick/Webs/macsurf/macTLS/os9/ostls_entropy.c:57)
- [macTLS/os9/ostls_fetch.c](/home/patrick/Webs/macsurf/macTLS/os9/ostls_fetch.c:203)
- [macTLS/os9/ostls_async.c](/home/patrick/Webs/macsurf/macTLS/os9/ostls_async.c:516)

Details:

- The entropy implementation explicitly states it is an insecure Stage A stub and “not enough for real security”.
- That same function is called in `OSTLS_Fetch` and in the async TLS setup path before `br_ssl_client_reset`.

Impact:

- TLS session security is materially undermined.
- Certificate validation may work, but confidentiality depends on usable randomness; this code explicitly does not provide it.

Recommendation:

- Do not ship or advertise these TLS paths as production-secure until a real entropy source exists.
- Gate insecure entropy behind explicit test-only build flags if the library is meant for real use.

### 6. Medium: Desktop debug log stores full browsing targets and request data

Evidence:

- [browser/netsurf/frontends/macos9/macsurf_debug_log.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macsurf_debug_log.c:4)
- [browser/netsurf/frontends/macos9/window.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/window.c:157)
- [browser/netsurf/frontends/macos9/window.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/window.c:208)
- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:389)
- [browser/netsurf/frontends/macos9/macos9_http_fetcher.c](/home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/macos9_http_fetcher.c:430)

Details:

- The browser writes a persistent Desktop log.
- Navigation input, cleaned URLs, proxy URLs, and direct fetch targets are logged.

Impact:

- Local privacy leak on shared or inspected systems.
- URLs can contain tokens, search terms, or internal paths.

Recommendation:

- Disable full-URL logging by default in normal builds.
- Redact query strings and credentials.
- Make persistent logging opt-in.

### 7. Medium: Proxy credential handling is operationally unsafe

Evidence:

- [proxy/main.go](/home/patrick/Webs/macsurf/proxy/main.go:17)
- [proxy/README.md](/home/patrick/Webs/macsurf/proxy/README.md:21)
- [proxy/README.md](/home/patrick/Webs/macsurf/proxy/README.md:48)
- [proxy/README.md](/home/patrick/Webs/macsurf/proxy/README.md:67)

Details:

- Credentials are passed via `--auth user:password`.
- The docs recommend starting the service with inline credentials on the shell command line.

Impact:

- Secrets can leak via shell history, process listings, service inspection, and copy-pasted deployment logs.

Recommendation:

- Read credentials from a file or environment variable with restrictive permissions.
- Stop documenting command-line secrets as the standard deployment path.

## Structural / Code Quality Notes

- `proxy/macsurf-proxy` is a tracked compiled binary. That is poor repository hygiene and makes review harder because source and artifact are mixed.
- The Classic Mac frontend contains several old fixed-size buffer patterns and hand-rolled protocol parsing paths. Even where they are not immediately exploitable, they raise audit and maintenance cost.
- The repo includes a large amount of vendor and experiment material; keeping first-party production code more clearly separated would make future security review easier.

## Secrets Review

Confirmed during this pass:

- No obvious hardcoded API keys or private keys were found in tracked first-party source.
- The CA bundle under `browser/netsurf/resources/ca-bundle` is expected certificate material, not a secret.
- The main secret-handling problem is operational: proxy credentials are designed to be passed on the command line and may then be logged or exposed externally.

## Overall Assessment

Current security posture is not production-safe if the proxy is Internet-reachable or if `macTLS` is relied on for real confidentiality.

The most urgent fixes are:

1. Lock down the proxy.
2. Add body-size limits in the proxy.
3. Fix the Mac OS 9 fetcher buffer handling.
4. Remove the insecure entropy path from any real TLS usage.
