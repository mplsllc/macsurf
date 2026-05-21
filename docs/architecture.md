# MacSurf Architecture
## A Modern Web Platform for Classic Mac OS 9

**Version:** 0.2-draft  
**Status:** Design  
**Author:** MPLS LLC  
**License:** MPLS License

---

## 1. Vision

MacSurf is a web platform for Classic Mac OS 9. Not a toy, not a novelty, a real browser
that loads real websites including Facebook and YouTube, on hardware from 1997 to 2002,
in a way that is seamless to the user.

The user opens MacSurf, types facebook.com, and their feed loads. That is the bar.

To get there, MacSurf is three things working together:

1. **A native Mac OS 9 browser**, a Carbon application built with CodeWarrior 8,
   running on real hardware or SheepShaver, with a rendering pipeline tuned for
   cooperative multitasking and QuickDraw.

2. **A smart proxy**, a Go server running on Hetzner that mediates between the
   Mac and the modern web. It strips TLS, rewrites content, executes JavaScript
   server-side, and streams rendered output back to the Mac in formats it can handle.

3. **A template layer**, a community-maintained, Git-backed library of site-specific
   rendering rules. When you load facebook.com, a Facebook template runs on the proxy,
   fetches your actual content, and delivers a MacSurf-native layout back to the Mac.
   Templates are versioned, open for contribution, and self-hostable.

These three components are not alternatives, they work together on every page load.
The rendering mode (Classic / Standard / Full) controls how much of the pipeline runs
on the Mac vs. on the proxy.

---

## 2. Rendering Modes

MacSurf exposes three rendering modes, selectable in Preferences. The right mode depends
on the machine and the user's preference for speed vs. fidelity.

### 2.1 Classic Mode

**Who it's for:** Slower hardware (G3, older G4), maximum compatibility, proxy does the
heavy lifting.

**How it works:**

1. Mac sends URL to proxy
2. Proxy checks for a matching template
3. If template exists: template fetches page content, executes JS server-side, builds a
   clean simplified HTML document with the user's actual content, sends it to the Mac
4. If no template: proxy fetches the page, runs it through a general-purpose SSR pass,
   strips incompatible content, sends simplified HTML
5. Mac renders the HTML using the native NetSurf pipeline (HTML parser → CSS → layout
   → QuickDraw plotters)

JS runs on the proxy. Images are resampled to PICT/GIF at the proxy before delivery.
The Mac never executes JavaScript.

**Result:** Fast, lightweight, works on any OS 9 machine. Layout fidelity depends on
the template quality.

### 2.2 Standard Mode

**Who it's for:** Mid-range hardware (400MHz+ G4), balance of speed and fidelity.

**How it works:**

Same pipeline as Classic, but the proxy delivers a richer HTML payload. The Mac
executes a limited subset of JavaScript natively via Duktape, enough to handle
form interactions, basic DOM manipulation, and simple UI state without round-tripping
to the proxy for every click.

**JS tier:** DOM manipulation, event handlers, form validation, simple state. No
network-dependent JS (fetch, XHR, WebSocket). Those fall back to proxy-side execution.

**Result:** Pages feel more interactive than Classic. Facebook works including likes
and comments. YouTube delivers video via the proxy's transcoded stream.

### 2.3 Full Mode

**Who it's for:** High-end machines, SheepShaver users, people who want to push it.

**How it works:**

The Mac executes as much JavaScript as Duktape can handle. The proxy is still in the
loop for TLS and template content, but the JS execution burden moves to the client.
More complex interactions work locally.

**JS tier:** Full Duktape execution. `fetch` and `XHR` route through Open Transport
via a bridge layer. DOM events handled locally. Falls back to proxy for anything that
crashes or times out.

**Result:** Closest to a real browser experience on the hardware. Some sites will be
slow on real hardware but fast on SheepShaver.

---

## 3. The Proxy

The current proxy (`proxy/`) is a 5-file, stdlib-only Go binary. It does TLS stripping
and CONNECT tunneling. It does not inspect or transform content.

The v0.2 proxy expands into multiple cooperating Go services on the same Hetzner host.
Each is a separate binary with a narrow responsibility.

### 3.1 Services

```
macsurf-gateway      Port 8765    Existing proxy, extended with routing and auth
macsurf-ssr          Internal     Server-side rendering engine (Node.js + Chromium)
macsurf-template     Internal     Template engine and Git-backed template runner
macsurf-transcode    Internal     Video/image transcoding for Mac-compatible formats
macsurf-stream       Internal     Pixel streaming (headless Chromium → screenshot loop)
```

All services communicate over localhost. The Mac connects only to `macsurf-gateway`
on port 8765. Routing is opaque to the client.

### 3.2 Gateway extensions

`macsurf-gateway` grows the following beyond what exists today:

- **Mode header:** Mac sends `X-MacSurf-Mode: classic|standard|full` with every
  request. Gateway routes accordingly.
- **Template lookup:** Before forwarding any request, gateway checks
  `macsurf-template` for a matching template. If found, the template handles the
  request instead of a raw upstream fetch.
- **Content rewriting:** For non-templated requests in Classic/Standard mode,
  gateway pipes the response through a general-purpose rewriter that strips
  incompatible content, rewrites absolute URLs, and inlines small images.
- **Image transcoding:** Images are passed through `macsurf-transcode` and
  delivered as GIF (for the widest OS 9 compatibility) or PICT.
- **Video routing:** Requests to YouTube or other video sources route to
  `macsurf-transcode`, which fetches the stream and delivers it as MPEG-1 or
  a QuickTime-compatible MPEG-4 at a bitrate the Mac can handle.

### 3.3 SSR engine

`macsurf-ssr` runs a headless Chromium instance via Playwright or Puppeteer. When
a page request arrives with no matching template:

1. Chromium fetches and renders the page
2. JS executes fully
3. The resulting DOM state is serialized to clean HTML
4. That HTML is passed back to the gateway for content rewriting and delivery

This is the fallback for any page without a template. It guarantees that any site
that works in a modern browser can be delivered to the Mac in some form.

### 3.4 Pixel streaming

`macsurf-stream` is the fallback of last resort. When SSR output is too complex for
the Mac to render (heavy CSS, canvas, WebGL), the mode can be switched to streaming:

1. Chromium renders the page at a fixed resolution matching the Mac's window size
2. A screenshot is taken on every meaningful change (navigation, scroll, input)
3. The screenshot is compressed and sent to the Mac as a PICT or raw bitmap
4. The Mac displays it in the browser window
5. Mouse clicks and keystrokes are sent back to the proxy as coordinates and events

The Mac becomes a thin client. The page is always current. Facebook works. YouTube
works via screenshot + audio stream (see §3.5).

This mode is intentionally not the default, it requires more server resources and
is slower to interact with. But it is the option that guarantees any page loads.

### 3.5 Video

YouTube and other video is handled in two passes:

**For Standard and Full mode:**
`macsurf-transcode` uses ffmpeg to fetch a video stream (via yt-dlp for YouTube),
transcode it to MPEG-1 or MPEG-4 Baseline at a Mac-appropriate bitrate, and deliver
it as a progressive HTTP stream. QuickTime on OS 9 opens the stream URL and plays it.

**For Classic mode and pixel streaming:**
Video plays in Chromium on the proxy. The frame loop captures each video frame as part
of the screenshot. Audio is delivered separately as a streaming MPEG-1 Audio file that
QuickTime opens alongside the screenshot window.

Specific bitrate targets (to be tuned against real hardware):
- G3 / slow G4: 320×240, 500kbps video, 64kbps audio
- Fast G4 / SheepShaver: 640×480, 1.5Mbps video, 128kbps audio

---

## 4. The Template System

Templates are the highest-leverage part of MacSurf. A well-written Facebook template
means Facebook always works, looks intentional, and feels fast, not like a degraded
accident.

### 4.1 What a template is

A template is a directory in the `macsurf-templates` Git repository:

```
templates/
  facebook.com/
    template.json      Metadata: name, version, author, domains, last-verified
    match.js           URL matching rules (which URLs this template handles)
    fetch.js           How to fetch content from the site (API calls, auth, etc.)
    render.js          How to produce MacSurf-native HTML from the fetched content
    assets/            Optional static assets (icons, CSS snippets)
  youtube.com/
    ...
  reddit.com/
    ...
```

Templates run in a sandboxed Node.js environment on `macsurf-template`. They have
access to authenticated session cookies (forwarded from the Mac), can make HTTP
requests to the origin site, and return a complete HTML document.

### 4.2 Template rendering contract

A template's `render.js` receives:

```javascript
{
  url: "https://facebook.com/",
  cookies: { ... },       // forwarded from the Mac's cookie jar
  mode: "classic",        // rendering mode from the Mac
  screen: { w: 800, h: 600 }  // Mac window size
}
```

It returns:

```javascript
{
  html: "...",            // complete HTML document
  title: "Facebook",
  cookies: { ... },       // updated cookies to send back to the Mac
  status: 200
}
```

The HTML it returns should use only tags and CSS that the Mac's NetSurf build
can handle. A template style guide (separate doc) will enumerate what's safe.

### 4.3 Template repository

The template repository lives at `github.com/mplsllc/macsurf-templates` (or
`forgejo.mp.ls/mplsllc/macsurf-templates` for self-hosters). It is:

- **Public**, anyone can read and clone
- **Contribution-based**, pull requests accepted for new templates and updates
- **Versioned**, each template has a `version` field and a `last-verified` date
- **Self-hostable**, the `macsurf-template` service accepts a `--templates-repo`
  flag pointing at any Git URL

The proxy polls the template repo on a configurable interval (default: 1 hour) and
hot-reloads changed templates without restart.

### 4.4 Template priority sites

These are the sites that matter most for initial template development, in priority
order:

1. Facebook (feed, profile, messages)
2. YouTube (homepage, video page, search)
3. Reddit
4. Wikipedia (already works well via SSR, template adds speed)
5. Gmail
6. Google Search
7. Twitter/X
8. Instagram

Each of these has a publicly documented API or a well-understood page structure.
Templates for structured sites (Reddit, Wikipedia, Google Search) are simpler than
social-graph sites (Facebook, Instagram).

---

## 5. The Native Browser

The Mac OS 9 native browser (`browser/netsurf/frontends/macos9/`) remains a
CodeWarrior 8 Carbon application built for PowerPC. The v0.1.0 pipeline
(OT fetch → strip → word-wrap → DrawText) is replaced by the full NetSurf
core pipeline.

### 5.1 Rendering pipeline (target state)

```
URL entered
  → macos9_http_fetcher (Open Transport)
  → fetch messages (FETCH_HEADER, FETCH_DATA, FETCH_FINISHED)
  → llcache / hlcache
  → html_init content handler (libhubbub parser)
  → DOM tree (libdom)
  → CSS cascade (libcss)
  → NetSurf layout engine
  → CONTENT_MSG_REDRAW
  → macos9 plotters (QuickDraw)
  → Window update
```

Every link in this chain requires real library implementations. The stub layer
that got us to v0.1.0 must be replaced.

### 5.2 Library dependency order

The libraries must be built for CodeWarrior 8 / C89 / PowerPC in this order
(each depends on the ones above it):

```
1. libwapcaplet     String interning (lwc_stub.c already implements this — done)
2. parserutils      Character encoding, input streams
3. libhubbub        HTML5 parser (depends on parserutils)
4. libdom           DOM implementation (depends on libhubbub, libwapcaplet)
5. libcss           CSS parser and cascade (depends on libwapcaplet, parserutils)
6. NetSurf core     html_init, nscss_init (depends on libdom, libcss)
```

This is the v0.2 milestone for the native browser. It is large. Each library
is a separate porting effort with its own C89 compliance pass and shim layer.

### 5.3 JavaScript, Duktape

Duktape is the JavaScript engine for Standard and Full modes. It is written in C,
designed to be embedded, and has been ported to constrained environments before.

The porting target is the same as the rest of the native build: C89, CodeWarrior 8,
PowerPC, no POSIX, cooperative multitasking.

The Duktape build is gated on the library dependency chain above, JS execution
requires a working DOM to be useful. It is a v0.3 milestone.

**JS tier implementation:**

- **Classic mode:** Duktape not loaded. `js_stub.c` remains active.
- **Standard mode:** Duktape loaded. DOM events, form handling, simple state.
  Network-dependent JS disabled (`fetch`, `XHR` stubbed to fail gracefully).
- **Full mode:** Duktape loaded with OT-backed `fetch` and `XHR` via a bridge
  in `macos9_fetch.c`. Full execution within Duktape's capability envelope.

### 5.4 Mode selection

Mode is selected in a MacSurf Preferences dialog (classic Mac OS modal, system font,
radio buttons). The selected mode is stored in a preferences file alongside the
application and sent as `X-MacSurf-Mode` on every proxy request.

A status indicator in the browser toolbar shows the active mode for the current page.
If the proxy falls back from a higher mode (e.g. SSR times out in Standard mode and
pixel streaming is used instead), the indicator updates to reflect the actual mode used.

### 5.5 Bookmarks and history

Both are v0.2 scope alongside the core rendering pipeline.

**Bookmarks:** Stored as a flat text file alongside the application (Mac HFS+ path).
Accessible from a Bookmarks menu. Add/remove/organize via a standard list dialog.
No folder hierarchy in v0.2, that is v0.3.

**History:** In-memory during the session. Written to a flat file on quit, loaded
on launch. Standard Back/Forward navigation via the existing toolbar buttons.
A History window (separate from the browser window) showing recent URLs is v0.3.

---

## 6. Milestones

### v0.2, Real rendering pipeline
- Port libwapcaplet (already done via lwc_stub.c), parserutils, libhubbub, libdom,
  libcss to C89 / CodeWarrior 8
- Replace all rendering stubs with real implementations
- Implement macos9 plotters (QuickDraw: clip, rectangle, text, bitmap)
- Implement macos9_http_fetcher backed by Open Transport
- Wire browser_window_create and netsurf_init into the main loop
- Proxy: template system, SSR engine, general-purpose content rewriter
- Template: facebook.com v1, youtube.com v1 (Classic mode only)
- Bookmarks and history (basic)

### v0.3, JavaScript and interactivity
- Port Duktape to C89 / CodeWarrior 8
- Standard mode JS tier (DOM events, form handling)
- Full mode JS tier (OT-backed fetch/XHR bridge)
- Pixel streaming mode (macsurf-stream service)
- Video transcoding (macsurf-transcode + QuickTime stream)
- History window, bookmark folders
- Template: reddit.com, google.com, gmail.com

### v0.4, Community and polish
- Template repository public launch
- Self-hosting documentation
- macsurf-templates contribution guide and style guide
- Per-template update notifications in the browser
- Offline mode (proxy cache, templates cached locally)
- Additional templates: twitter.com, wikipedia.com, instagram.com

---

## 7. Infrastructure

All services run on the existing Hetzner host (`116.202.231.103`). Current load is
negligible (5.3 MB RSS for the proxy after 3 days). The host has capacity for
Chromium headless (SSR and streaming) and ffmpeg (transcoding).

Each service is a separate systemd unit under the `macsurf` user. The existing
`macsurf-proxy.service` pattern is the template for all new units.

Service communication is over `127.0.0.1` only. The gateway is the sole public-facing
process.

Self-hosters run the same stack via Docker Compose. A `docker-compose.yml` at the
repo root starts all services with sane defaults.

---

## 8. Open questions

These are not answered by this document and require decisions before implementation:

1. **Session / cookie handling:** How does the Mac persist cookies across sessions?
   The proxy needs to forward them to templates. Does the Mac store them locally and
   send them with every request, or does the proxy maintain a session store keyed to
   the Mac's IP or a session token?

2. **Authentication:** Facebook and Gmail require login. How does the user authenticate?
   Options: (a) the Mac sends credentials to the template which logs in on their
   behalf, (b) pixel streaming is used for login pages and Classic/Standard resumes
   after auth, (c) session cookies are imported from another browser.

3. **HTTPS from the Mac:** Currently the Mac sends plain HTTP to the proxy and the
   proxy handles TLS upstream. Is that acceptable long-term, or does the Mac-to-proxy
   connection need encryption? (The connection is over the public internet today.)

4. **Template sandbox security:** Templates run JS on the proxy. What is the sandbox
   boundary? Can a malicious template exfiltrate the user's cookies from another site?

5. **Pixel streaming resolution:** What resolution does the Mac browser window actually
   support? The screenshot from Chromium must match it exactly or the bitmap fill
   will be wrong.

6. **Audio delivery:** What audio formats can QuickTime on OS 9 stream over HTTP?
   MPEG-1 Audio Layer 3 (MP3) should work. Does it handle chunked HTTP, or does it
   need a complete file?

---

## 9. What this is not

MacSurf is not trying to make OS 9 a general-purpose modern computing platform.
It is a browser for people who love classic Mac hardware and want to use it. The proxy
and template layer exist in service of that goal, they are infrastructure, not the
product. The product is the experience of opening MacSurf on a beige G3, going to
Facebook, and seeing your actual feed.

Every architectural decision should be evaluated against that bar.