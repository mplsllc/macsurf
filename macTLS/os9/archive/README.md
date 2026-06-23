# os9/archive/ — historical references, not in the build

Source files in this folder are **not part of the MacTLSTest project
target** and should not be added to MacTLSTest.mcp. They are kept
under version control because the git history of how they evolved is
useful negative reference material, and because each carries a
documented finding about classic Mac OS 9 that future contributors
should not have to re-discover.

## What's here

### `ostls_c1_listener_carbon_cfm_abandoned.{h,c}`

Stage C1 — the local TCP listener for the never-shipped "macTLS Proxy"
architecture. **DO NOT re-enable this code in the build unless
targeting a non-Carbon CFM build (MPW / pre-Carbon OT) or a different
process model.**

The 14 fixes (fixes16..fixes34) of investigation around this file
established conclusively that Carbon CFM apps using
`OTOpenEndpointInContext` cannot bind a passive endpoint to a
caller-chosen `InetAddress`. Every combination of address / port /
qlen / sync vs async / protocol stack returns `kOTBadAddressErr (-3150)`.

Full investigation report: [docs/carbon-ot-passive-bind-finding.md](../../docs/carbon-ot-passive-bind-finding.md).

If a future macTLS fork ever targets:

- MPW C with the non-InContext OT API (e.g. an MPW shell tool / inetd
  style daemon), OR
- a different runtime model (e.g. classic 68K via Retro68 with raw OT
  rather than the Carbon wrapper)

then this file is a reasonable starting point — much of the bind /
listen / accept structure is correct in principle and only the
`OTOpenEndpointInContext` call needs to change. But for **Carbon CFM
on classic Mac OS 9, this code can never work** and re-adding it to
the build is wasted compile time.

### `ostls_b4_https_get_superseded.{h,c}`

The Stage B4 standalone HTTPS GET probe. Superseded by
`os9/ostls_fetch.c` which is the same logic with the public library
signature (`OSTLS_Fetch(host, port, server_name, path, ...)`).

Kept here for git-history comparison; not in the build target.
