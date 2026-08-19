# macTLS — native TLS 1.3 stack

Hand-written TLS 1.3 (BearSSL primitives underneath) for direct HTTPS from the Mac, no
proxy. `os9/` is the Mac OS 9 integration layer.

## Certificate time validation

- **`OSTLS_GetBearSSLTime` (`os9/ostls_time.c`) must convert the Mac's local
  `GetDateTime()` reading to GMT via `ReadLocation`'s `gmtDelta` before handing it to
  `br_x509_minimal_set_time`.** Passing local time as if it were GMT makes a freshly-issued
  certificate (whose `notBefore` falls within the timezone offset) read as not-yet-valid on
  any machine set to a timezone behind GMT — older certs are unaffected, which produces the
  maddening "only the fresh cert, only on this one machine" signature. This depends on the
  Mac's Date & Time control panel actually having its Time Zone set.

## Reading a TLS handshake failure

- `hs_state` = how far the handshake got (5 = RecvCertificate).
- `br_err` = why (54 = `X509_EXPIRED` — covers BOTH expired and not-yet-valid; 55 =
  `DN_MISMATCH`).
