# macLink

**System-wide TLS bridge for Classic Mac OS.**

macLink is a Faceless Background Application that runs a local TLS-terminating proxy on the Mac itself. Once installed (and its root CA imported into each TLS-using app, one time per app), every classic Mac application that respects Internet Config's proxy setting — Eudora, Outlook Express 5, Internet Explorer 5, Netscape 7, Fetch, Anarchie, NCSA Telnet, Sherlock, and many others — speaks **modern TLS** to modern servers, using macTLS (the same TLS engine MacSurf already ships).

No remote service. No telemetry. No shared proxy IP. Each Mac is its own TLS authority, scoped to its own apps, talking to the modern web from its own IP address.

## Sister projects

- [MacSurf](https://github.com/mplsllc/macsurf) — the modern browser
- [macTLS](https://github.com/mplsllc/macTLS) — the TLS 1.2/1.3 engine
- macEntropy (in macTLS/os9) — the HMAC-DRBG random source
- **macLink** — *(this project)* the system bridge
- macAuth — *(future)* OAuth 2.0 bridge for Gmail/Outlook personal accounts
- macMail — *(future)* native IMAP/SMTP client with HTML email rendering
- macFTP — *(future)* native FTP/FTPS client

## Project status

**Phase 0** — seed shipped; awaiting CW8 build + hardware test on a G3/G4 running OS 9.2.2.

See [PHASE_0.md](PHASE_0.md) for the immediate test plan and pass criteria.

See [REUSE_MAP.md](REUSE_MAP.md) for the full file-by-file inventory of what we lift from macTLS, MacSurf, and BearSSL versus what's net-new in macLink.

See [`docs/research/maclink-scope.md`](../docs/research/maclink-scope.md) in the macsurf repo for the full architectural scope.

## Documents

- [PHASE_0.md](PHASE_0.md) — Phase 0 deliverables, CW8 project setup, hardware test plan
- [REUSE_MAP.md](REUSE_MAP.md) — file-by-file reuse inventory

## License

Same terms as macTLS and MacSurf (parent project — TBD; will mirror those decisions once they finalize).
