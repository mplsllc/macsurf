# Contributors

## MacSurf

- **Patrick** — author and maintainer of MacSurf (NetSurf for Classic Mac OS 9,
  with the native macTLS stack and the on-device QuickJS engine).

## Credited for parallel / convergent work

- **Sempai Squad** — [@sempaisquad](https://github.com/sempaisquad). Author of
  **ClassicNetSurf**, a concurrent NetSurf-on-Classic-Mac-OS port developed
  independently at a different scope. MacSurf reached its QuickJS DOM-wrapper
  lifecycle design (node-identity map + owner-document keepalive + finalizer,
  `macsurf_qjs.c`) on its own, but ClassicNetSurf's hand-written `quickjs.c`
  arrived at the same discipline in parallel — credited here for that work.

  _Convention:_ this project says **"ported"** only for code lifted directly
  from another source; work MacSurf reached before reading that source is
  **convergent / independently arrived at**, and the other author is still
  credited for having solved it too.
