# MacSurf: The Modern Web Moonshot
## Bridging the Gap Between OS 9 and the Modern Web

**Date:** 2026-04-23
**Status:** Research & Scoping

---

## 1. The Reality Gap

To load modern websites like Facebook, YouTube, or modern web apps, a browser needs four major capabilities that MacSurf currently lacks natively:

1.  **ES6+ JavaScript Execution:** Modern sites ship ES2015+ syntax (arrow functions, `async`/`await`, classes). Duktape (the JS engine available for MacSurf) only supports ES5.1. A modern JS file will instantly throw a syntax error in Duktape and halt execution.
2.  **Modern Web APIs:** Sites rely heavily on `fetch`, `Promises`, `WebSockets`, `MutationObserver`, `LocalStorage`, etc. NetSurf's built-in Duktape bindings are extremely basic (DOM Level 2/3).
3.  **Advanced CSS:** NetSurf's `libcss` has decent CSS 2.1 and partial Flexbox support, but treats CSS Grid as `block`. It completely lacks support for CSS Variables (`var(--color)`), `calc()`, and modern pseudo-selectors, which breaks layout on 90% of modern sites.
4.  **Modern Media:** WebP, AVIF, MP4, and WebM are standard. Mac OS 9 QuickDraw only natively understands PICT, and we can decode JPEG/GIF/BMP.

We cannot build a modern browser engine from scratch in C89 running on a 400MHz PowerPC G4 with 64MB of RAM. **We must leverage the MacSurf Proxy to do the heavy lifting.**

---

## 2. The Architecture Split

The core strategy is to make the Proxy act as a "time machine," translating the modern web into the ES5 / CSS2.1 / JPEG web that MacSurf can natively understand.

### Client-Side (MacSurf / CodeWarrior 8 / OS 9)
*   **JS Engine (Duktape):** We will wire up Duktape and expand its C bindings. We must implement `document.querySelector`, `classList`, and a native hook for HTTP requests (to support XHR/Fetch polyfills) routed through Open Transport.
*   **Garbage Collection:** Duktape's mark-and-sweep GC will cause UI pauses on a G4. We must wire the GC step to run incrementally during the `WaitNextEvent` idle loop (`TEIdle` phase).
*   **Layout:** Stick with NetSurf's Hubbub & LibCSS. We will fix any glaring bugs in NetSurf's Flexbox implementation, but we rely on the proxy for Grid fallbacks.

### Server-Side (MacSurf Proxy / Go)
*   **JS Transpilation (Babel/ESBuild):** The proxy MUST intercept all `.js` network requests and inline `<script>` tags. It will parse the AST, transpile ES6+ syntax down to ES5.1, and serve the ES5 version to MacSurf.
*   **Polyfill Injection:** The proxy will automatically inject an ES5 polyfill bundle (like `core-js` and `whatwg-fetch`) into the `<head>` of every HTML document.
*   **CSS Downgrading (PostCSS):** The proxy intercepts `.css` files and `<style>` blocks. It runs PostCSS to:
    *   Resolve CSS Variables (`var(--color)`) into static hex values.
    *   Resolve `calc()` expressions statically where possible.
    *   Apply Flexbox fallbacks for CSS Grid where possible.
*   **Media Transcoding:** Intercept `image/webp` and `image/avif` and transcode them to baseline `image/jpeg` or `image/gif` on the fly using Go's `image` standard libraries.

---

## 3. Implementation Roadmap

### Phase A: The ES5 Foundation (Mac OS 9 side)
1.  **Enable Duktape:** Un-stub `js_stub.c`, compile Duktape into the NetSurf event loop (`macsurf_js_pump_all()`).
2.  **DOM Bindings:** Implement the missing Duktape-to-LibDOM bindings in C. Ensure `getElementById`, `addEventListener`, `innerHTML`, and style modifications work.
3.  **Network JS:** Implement an XHR/Fetch shim in Duktape that calls our Open Transport backend (`macos9_fetch.c`).
4.  **Incremental GC:** Bind `duk_gc` to the cooperative event loop to prevent freezing.

### Phase B: The Modernization Pipeline (Proxy side)
1.  **Transpiler Engine:** Add a Go integration (or a sidecar Node.js/esbuild process) to the MacSurf Proxy. When a JS file is requested, the proxy fetches it, transpiles to ES5, caches it, and serves it.
2.  **CSS Processor:** Integrate a CSS parser in the proxy to flatten CSS variables. This is the #1 cause of "invisible text" on modern sites when viewed in older browsers.
3.  **Image Transcoding:** Add HTTP interception for images; if the upstream returns WebP, convert the buffer to JPEG before sending it down the wire to the Mac.

### Phase C: Site-Specific Templates (The Nuclear Option)
For sites like YouTube or Facebook where transpiling React/Next.js to ES5 is simply too slow for a G4 processor, we fall back to **Templates**.
*   The proxy detects `youtube.com`.
*   Instead of serving YouTube's 5MB of transpiled JS, the proxy fetches the YouTube data via API.
*   The proxy generates a static HTML4/CSS2.1 representation of the video page.
*   MacSurf renders the native, lightning-fast static page.

---

## Conclusion
We have the HTML/CSS parsing (NetSurf) and the JS engine (Duktape). What we are missing is **Modern Web Translation**. By extending the Go Proxy to downgrade JS, CSS, and Images on the fly, MacSurf will be able to load modern sites without requiring a Chromium port to OS 9.
