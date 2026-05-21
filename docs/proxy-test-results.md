# MacSurf Proxy Test Results

Tested 2026-04-07 after fixing WriteTimeout and Dockerfile issues.

## Changes Made

1. **Removed `WriteTimeout` from server config**, the 60s WriteTimeout killed long-running CONNECT tunnels. Replaced with a 10-minute idle timeout in the `transfer()` function using `SetDeadline` on both connections, reset on each data transfer.

2. **Updated Dockerfile**, `CGO_ENABLED=0 go build` for a fully static binary, final image changed from alpine to `scratch`. CA certificates copied from build stage for HTTPS upstream fetches.

---

## Test 1, HTTP proxy (plain HTTP target)

```
$ curl -v -x http://localhost:8765 http://example.com
* Host localhost:8765 was resolved.
* IPv6: ::1
* IPv4: 127.0.0.1
*   Trying [::1]:8765...
* Connected to localhost (::1) port 8765
> GET http://example.com/ HTTP/1.1
> Host: example.com
> User-Agent: curl/8.5.0
> Accept: */*
> Proxy-Connection: Keep-Alive
> 
< HTTP/1.1 200 OK
< Age: 11106
< Allow: GET, HEAD
< Cf-Cache-Status: HIT
< Cf-Ray: 9e8bd3d89dabdbf2-FRA
< Connection: keep-alive
< Content-Type: text/html
< Date: Tue, 07 Apr 2026 20:36:58 GMT
< Last-Modified: Tue, 24 Mar 2026 22:06:31 GMT
< Server: cloudflare
< Transfer-Encoding: chunked
< 
<!doctype html><html lang="en"><head><title>Example Domain</title>...
* Connection #0 to host localhost left intact
```

**Result: PASS**, HTTP 200, full HTML body returned.

---

## Test 2, HTTPS CONNECT tunnel (example.com)

```
$ curl -v -x http://localhost:8765 https://example.com
*   Trying [::1]:8765...
* Connected to localhost (::1) port 8765
* Establish HTTP proxy tunnel to example.com:443
> CONNECT example.com:443 HTTP/1.1
> Host: example.com:443
> User-Agent: curl/8.5.0
> Proxy-Connection: Keep-Alive
> 
< HTTP/1.1 200 OK
* CONNECT tunnel established, response 200
* SSL connection using TLSv1.3 / TLS_AES_256_GCM_SHA384 / X25519 / id-ecPublicKey
* Server certificate:
*  subject: CN=example.com
*  issuer: C=US; O=CLOUDFLARE, INC.; CN=Cloudflare TLS Issuing ECC CA 1
*  SSL certificate verify ok.
* using HTTP/2
> GET / HTTP/2
> Host: example.com
> 
< HTTP/2 200 
< content-type: text/html
< server: cloudflare
< 
<!doctype html><html lang="en"><head><title>Example Domain</title>...
* Connection #0 to host localhost left intact
```

**Result: PASS**, CONNECT tunnel established, TLS 1.3 negotiated end-to-end through proxy, HTTP/2 200 response.

---

## Test 3, HTTPS CONNECT tunnel (macintoshgarden.org)

```
$ curl -v -x http://localhost:8765 https://macintoshgarden.org
*   Trying [::1]:8765...
* Connected to localhost (::1) port 8765
* Establish HTTP proxy tunnel to macintoshgarden.org:443
> CONNECT macintoshgarden.org:443 HTTP/1.1
> Host: macintoshgarden.org:443
> User-Agent: curl/8.5.0
> Proxy-Connection: Keep-Alive
> 
< HTTP/1.1 200 OK
* CONNECT tunnel established, response 200
* SSL connection using TLSv1.3 / TLS_AES_256_GCM_SHA384 / secp384r1 / RSASSA-PSS
* Server certificate:
*  subject: CN=*.macintoshgarden.org
*  issuer: C=GB; O=Sectigo Limited; CN=Sectigo Public Server Authentication CA DV R36
*  SSL certificate verify ok.
* using HTTP/2
> GET / HTTP/2
> Host: macintoshgarden.org
> 
< HTTP/2 200 
< server: nginx
< content-type: text/html; charset=utf-8
< x-powered-by: PHP/7.4.33
< 
[34KB HTML body — Macintosh Garden homepage]
* Connection #0 to host localhost left intact
```

**Result: PASS**, CONNECT tunnel to real-world site, TLS 1.3, full page returned.

---

## Test 4, Auth: unauthenticated request (expect 407)

```
$ curl -v -x http://localhost:8765 http://example.com
# Proxy started with: --auth test:password
*   Trying [::1]:8765...
* Connected to localhost (::1) port 8765
> GET http://example.com/ HTTP/1.1
> Host: example.com
> User-Agent: curl/8.5.0
> Accept: */*
> Proxy-Connection: Keep-Alive
> 
< HTTP/1.1 407 Proxy Authentication Required
< Content-Type: text/plain; charset=utf-8
< Proxy-Authenticate: Basic realm="macsurf-proxy"
< X-Content-Type-Options: nosniff
< Date: Tue, 07 Apr 2026 20:37:23 GMT
< Content-Length: 30
< 
Proxy authentication required
* Connection #0 to host localhost left intact
```

**Result: PASS**, 407 with `Proxy-Authenticate` header, request blocked.

---

## Test 5, Auth: authenticated request (expect 200)

```
$ curl -v -x http://test:password@localhost:8765 http://example.com
*   Trying [::1]:8765...
* Connected to localhost (::1) port 8765
* Proxy auth using Basic with user 'test'
> GET http://example.com/ HTTP/1.1
> Host: example.com
> Proxy-Authorization: Basic dGVzdDpwYXNzd29yZA==
> User-Agent: curl/8.5.0
> Accept: */*
> Proxy-Connection: Keep-Alive
> 
< HTTP/1.1 200 OK
< Age: 11131
< Allow: GET, HEAD
< Cf-Cache-Status: HIT
< Cf-Ray: 9e8bd476eabd1ac5-FRA
< Connection: keep-alive
< Content-Type: text/html
< Date: Tue, 07 Apr 2026 20:37:23 GMT
< Last-Modified: Tue, 24 Mar 2026 22:06:31 GMT
< Server: cloudflare
< Transfer-Encoding: chunked
< 
<!doctype html><html lang="en"><head><title>Example Domain</title>...
* Connection #0 to host localhost left intact
```

**Result: PASS**, Basic auth accepted, proxied request returned 200.

---

## Summary

| Test | Target | Method | Auth | Expected | Actual | Status |
|------|--------|--------|------|----------|--------|--------|
| 1 | example.com | HTTP GET | None | 200 | 200 | PASS |
| 2 | example.com | CONNECT | None | 200 | 200 | PASS |
| 3 | macintoshgarden.org | CONNECT | None | 200 | 200 | PASS |
| 4 | example.com | HTTP GET | Required, not sent | 407 | 407 | PASS |
| 5 | example.com | HTTP GET | Required, sent | 200 | 200 | PASS |

All 5 tests passed. The proxy correctly handles HTTP forwarding, HTTPS CONNECT tunneling, and basic authentication.
