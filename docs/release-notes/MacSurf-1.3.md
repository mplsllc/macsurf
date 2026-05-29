# MacSurf 1.3 — Forward

**Released:** 2026-05-29
**Codename:** Forward
**Engine HEAD:** fixes315 (MacSurf side); macTLS at `tls13-v1` (c405117)
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.2.2

<p align="center">
  <img src="https://raw.githubusercontent.com/mplsllc/macsurf/master/img/mactls-logo.png" alt="macTLS" width="420">
</p>

---

## The headline

**MacSurf 1.3 negotiates TLS 1.3 natively on Classic Mac OS.** No proxy. No helper machine. The ClientHello, the key schedule, the record layer, and the decrypted application data all happen on a 233 MHz Power Macintosh G3 iMac running Mac OS 9.2.2.

As far as we can find, this is the **first native TLS 1.3 implementation on Classic Mac OS, ever.** It shipped less than 24 hours after the v1.2 "Sealed" release that closed the entropy hole. The cryptographic foundation is now genuinely modern from the ClientHello bytes all the way to the application data.

---

## Third-party verification

This isn't an internal claim. Three independent test sites confirm TLS 1.3 negotiation from MacSurf on real hardware.

### Akamai (`tls13.akamai.io`)

![Akamai TLS 1.3 verification](https://raw.githubusercontent.com/mplsllc/macsurf/master/screenshots/macsurf-1.3-akamai-tls13.jpg)

Akamai's TLS 1.3 endpoint reports:
- `TLS Version: tls1.3`
- `TLS Cipher Name: TLS_CHACHA20_POLY1305_SHA256`
- `User Agent: MacSurf/0.2 (Macintosh; PPC Mac OS 9)`
- **"Your client negotiated TLS 1.3, the latest version of the TLS protocol!"**

### BrowserLeaks (`browserleaks.com/tls`)

![BrowserLeaks TLS fingerprint](https://raw.githubusercontent.com/mplsllc/macsurf/master/screenshots/macsurf-1.3-browserleaks-tls13.jpg)

- `TLS Protocol: 0x0304 TLS 1.3`
- `Cipher Suite: 0x1303 TLS_CHACHA20_POLY1305_SHA256 [Recommended] TLS 1.3`
- `Key Exchange: 0x001D x25519`
- `Signature Scheme: 0x0804 rsa_pss_rsae_sha256`
- Supported cipher suites advertised in ClientHello include both TLS 1.3 (`0x1303`, `0x1301`) and TLS 1.2 fallback suites (`0xCCA9`, `0xCCA8`, `0xC02B`, `0xC02F`, `0xC02C`, `0xC030`).

### How's My SSL (`howsmyssl.com`)

![How's My SSL report](https://raw.githubusercontent.com/mplsllc/macsurf/master/screenshots/macsurf-1.3-howsmyssl.jpg)

- **Version: Good** — "Your client is using TLS 1.3, the most modern version of the encryption protocol. It gives you access to the fastest, most secure encryption possible on the web."
- **Ephemeral Key Support: Good** — forward secrecy
- **TLS Compression: Good** — not vulnerable to CRIME
- **Insecure Cipher Suites: Good** — none present

### Cloudflare (`/cdn-cgi/trace`)

```
fl=398f405
h=cloudflare.com
ip=172.58.10.86
ts=1780073443.577
visit_scheme=https
uag=MacSurf/0.2 (Macintosh; PPC Mac OS 9)
colo=ORD
http=http/1.1
loc=US
tls=TLSv1.3
sni=plaintext
kex=X25519
```

Four independent third parties, in the actual browser, on a beige... actually no, snow white iMac G3 with Mac OS 9.2.2. The first native TLS 1.3 handshake any Classic Mac OS browser has ever completed.

---

## What's new under the hood

The TLS 1.3 work landed in macTLS (separate repo, tag `tls13-v1` at commit `c405117`). MacSurf picks it up automatically because the public macTLS API surface didn't change.

**macTLS TLS 1.3 implementation:**

- **Hand-written TLS 1.3 handshake, key schedule, and record layer** per [RFC 8446](https://datatracker.ietf.org/doc/html/rfc8446), built on BearSSL cryptographic primitives only. BearSSL doesn't ship TLS 1.3; this is a from-scratch 1.3 layer over its 1.2 building blocks.
- **X25519 key exchange** (curve25519 via BearSSL's `ec_c25519_m15`).
- **Cipher suites: `0x1303` (TLS_CHACHA20_POLY1305_SHA256) and `0x1301` (TLS_AES_128_GCM_SHA256).**
- **SHA-256 transcript hash** for the handshake.
- **Server authentication against the 121-anchor Mozilla CCADB bundle** that's been shipping since v0.6.
- **RFC 8446 + RFC 8448 test vectors pass** on host and on-device.

**Wire behaviour:**

Every outbound connection now opens with a TLS 1.3 ClientHello that also advertises TLS 1.2 cipher suites in the legacy field. The handshake outcome is decided by the server:

- Server selects TLS 1.3 → 1.3 handshake completes, 1.3 record layer engaged, payload flows over `0x1303` or `0x1301`.
- Server selects TLS 1.2 → macTLS reconnects and BearSSL's engine drives a full 1.2 handshake against the original 121-anchor bundle.

**The switch is invisible to callers.** `OSTLS_Read` / `OSTLS_Write` return the same plaintext either way. The MacSurf fetcher code shipped in v1.2 picked up TLS 1.3 with zero source changes. The fetcher still calls `OSTLS_New` / `OSTLS_Start` / `OSTLS_Pump` / `OSTLS_Read` / `OSTLS_Write`; everything below those calls is now modern.

`OSTLS_SetTryTLS13(0)` forces 1.2-only for diagnostics or compatibility.

**TLS 1.2 (sync, async, and session resumption) is unchanged and intact.** No regression risk on existing sites.

---

## What's NOT in this release

Documented for honest accounting and so the roadmap is clear.

- **TLS 1.3 session resumption** (pre-shared keys / session tickets). This is the CDN-resumption win. Cold-handshake 1.3 still costs ~ same wall-clock as 1.2 on G3 hardware; PSK resumption is what makes 1.3 dramatically faster on revisits. Queued for follow-on.
- **Post-quantum key agreement** (e.g. ML-KEM). Not in scope for 1.3.
- **TLS client certificates** (mutual TLS). Server-auth only.
- **0-RTT early data.** Adds attack surface; deferred.

---

## Credits

The TLS 1.3 work is adapted from **[Certainly](https://github.com/minorbug/Certainly)** by **minorbug** (MIT license), originally a C99 / Retro68 implementation. The macTLS port re-engineered it to **CodeWarrior 8 / strict C89** for the OS 9 build target.

**[BearSSL](https://www.bearssl.org/)** by **Thomas Pornin** (MIT license) provides the cryptographic primitives — AES-GCM, ChaCha20-Poly1305, SHA-256, curve25519 — that the 1.3 layer composes.

The 1.3 work is a single-author project on top of those two foundations. macTLS has been a community-adjacent effort from its v0.x days; this release crosses the line into "modern HTTPS, no asterisks" on a 1998 iMac.

---

## Building from source (Mac side)

The MacSurf binary picks up TLS 1.3 by including four additional files in MacSurf.mcp from the updated macTLS source tree:

- `bearssl/src/ec/ec_c25519_m15.c` (X25519 key exchange)
- `os9/ostls_tls13_keysched.c`
- `os9/ostls_tls13_record.c`
- `os9/ostls_tls13_handshake.c`

No other MacSurf-side code changes are required. The fetcher behavior is unchanged at the source level; the upgrade is entirely transparent through the macTLS public API.

If you're pulling 1.3 onto an existing 1.2-only MacSurf workspace, the four files above are the only additions. Build clean, watch the handshake go modern.

---

## Pacing note

v1.2 shipped 2026-05-28 closing the entropy hole. v1.3 ships 2026-05-29 with the modern protocol layer. **Less than 24 hours between releases.** This is genuinely uncommon for a project at this size and complexity; it reflects how the entropy work in v1.2 unlocked the 1.3 work that was already in flight at macTLS. Both pieces are now landed and shipping together as a coherent cryptographic foundation.

The protocol stack on the wire is now:

```
HTTP/1.1
  TLS 1.3 (CHACHA20-POLY1305 or AES-128-GCM, x25519, SHA-256)
    TCP (Open Transport)
      IPv4
        Ethernet (built-in on a 1998 iMac G3)
```

On a G3 iMac. In 2026.

---

*MacSurf is at [github.com/mplsllc/macsurf](https://github.com/mplsllc/macsurf). macTLS is at [github.com/mplsllc/macTLS](https://github.com/mplsllc/macTLS). The companion site is [home.macsurf.org](https://home.macsurf.org/). Bug reports and screenshots from real hardware are exactly what this project wants.*
