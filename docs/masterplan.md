# MacSurf: NetSurf for Classic Mac OS 9
### Master Plan & Roadmap

---

## Vision

A lightweight, usable web browser for Mac OS 9 PowerPC machines — built on the NetSurf engine, paired with a dead-simple TLS proxy that anyone can deploy. The goal: make a 1999 Power Mac G4 browse the modern web without fighting it.

**Core principles:**
- Low memory footprint — target under 16MB RAM usage
- Simple setup — proxy config built into preferences, not buried
- Respect the platform — feels like a Mac OS 9 app, not a port
- Open source, community maintainable

---

## Component 1: MacSurf Browser

### Overview
Port NetSurf to Classic Mac OS 9 using the Carbon API (for compatibility with both OS 9 and early OS X) with CodeWarrior as the compiler. Use the RISC OS and AmigaOS frontends as primary references — both solved the same cooperative multitasking problem.

NetSurf tabs: **disabled by default**, available as an opt-in preference. OS 9 users expect single-window or simple window-per-site behavior. Don't fight the platform.

---

### Phase 1 — Build Environment

**Goal:** Get NetSurf's core libraries compiling for PowerPC.

Tasks:
- Set up cross-compilation toolchain on Linux targeting PowerPC Mac OS 9
- Target CodeWarrior 8 project format for native on-machine builds
- Port NetSurf's dependency libraries to PowerPC:
  - **Hubbub** (HTML parser)
  - **LibCSS** (CSS parser)
  - **libdom** (DOM)
  - **libparserutils**
  - **libwapcaplet**
- Strip or stub any POSIX dependencies not available in Classic Mac libc
- Verify each library compiles and passes its test suite under the toolchain

**Reference:** NetSurf's existing `Makefile.config` per-platform overrides.

---

### Phase 2 — Platform Abstraction Layer (Frontend)

**Goal:** Write the Classic Mac frontend that implements NetSurf's platform API.

NetSurf separates core from frontend cleanly. You implement a set of callbacks and the core does the rest. Create `frontends/macos9/` modeled on `frontends/riscos/` and `frontends/amiga/`.

Key frontend interfaces to implement:

| NetSurf Interface | Mac OS 9 Implementation |
|---|---|
| `gui_window_create` | `NewCWindow` / `NewWindow` Toolbox calls |
| `gui_window_set_title` | `SetWTitle` |
| `gui_window_update_box` | `InvalRect` + redraw queue |
| `browser_window_navigate` | Trigger Open Transport fetch |
| `gui_launch_url` | Open in new window |
| `gui_32bpp_to_bitmap` | `CopyBits` to GWorld |
| Scrolling | `ScrollRect`, scroll bar controls |
| Menus | Mac Toolbox menu manager |
| Clipboard | `ZeroScrap` / `PutScrap` |
| Font rendering | QuickDraw text + ATM for smooth fonts |

---

### Phase 3 — Cooperative Multitasking

**Goal:** Make NetSurf's async model work inside OS 9's cooperative event loop.

This is the hardest phase. NetSurf assumes it can block or thread. OS 9 does not preempt — you must yield control regularly via `WaitNextEvent`.

Strategy:
- Implement a simple coroutine/continuation scheduler around `WaitNextEvent`
- Break NetSurf's fetch and layout operations into resumable chunks
- Each pass through the event loop does a small unit of work (fetch a buffer, parse a chunk, lay out N lines)
- Network I/O via Open Transport in async (non-blocking) mode with completion callbacks
- Rendering broken into dirty-rect invalidation passes, not full redraws

Reference: How Classilla solved this same problem (it's open source, study it).

---

### Phase 4 — Networking

**Goal:** Feed NetSurf's fetcher layer via Open Transport.

- Replace NetSurf's `curl`-based fetcher with a custom Open Transport fetcher
- Implement HTTP/1.1 over OT directly (GET, POST, headers, chunked transfer)
- HTTPS handled entirely by the proxy (see Component 2) — browser only speaks plain HTTP to localhost or proxy host
- DNS via OT's `OTInetStringToAddress`
- Connection pooling: keep-alive support for performance on slow connections

---

### Phase 5 — Rendering

**Goal:** Draw pages correctly using QuickDraw.

- NetSurf's Hubbub + LibCSS handles parsing — frontend just needs to draw
- Use GWorlds for offscreen rendering, then `CopyBits` to screen
- Images: decode to 32-bit GWorld, then convert to screen depth
- Text: QuickDraw `DrawString` for basic, ATM for smooth font rendering
- CSS color/font/layout handled by NetSurf core — frontend draws primitives only

---

### Phase 6 — UI & UX

**Goal:** Feel like a real Mac OS 9 app.

- Menu bar: File, Edit, View, Go, Bookmarks, Help
- Location bar at top (editable text field)
- Back/Forward/Stop/Reload buttons (use Appearance Manager for proper OS 9 look)
- Status bar at bottom showing load progress and URL on hover
- **Tabs: off by default.** If enabled in prefs, implement as a simple tab bar above content area — no fancy animations, just buttons
- Preferences dialog with proxy settings front and center (see Component 2 integration)
- Bookmarks stored as simple alias file or flat text file in Preferences folder
- Error pages rendered as simple styled HTML, no blank screens

---

### Phase 7 — Proxy Integration

**Goal:** Make connecting to MacSurf Proxy dead simple.

In Preferences → Proxy:
- Host field (default: `localhost`)
- Port field (default: `8765`)
- "Test Connection" button
- One-click enable/disable

If proxy is unreachable, show a clear human-readable error — not a network code.

---

### Memory & Performance Targets

| Metric | Target |
|---|---|
| Base RAM usage | < 16MB |
| RAM with page loaded | < 32MB |
| Minimum system | Power Mac G3, OS 9.1 |
| Recommended | G4 300MHz+, 128MB RAM, OS 9.2.2 |
| JS support | None (by design) |
| CSS support | CSS 1 + partial CSS 2 via LibCSS |

---

## Component 2: MacSurf Proxy

### Overview

A small, single-binary TLS stripping proxy written in Go. Deployed on a VPS or run locally. Receives plain HTTP from the Mac, fetches via HTTPS, strips TLS, returns plain HTTP. No configuration files, no databases, no dependencies.

**Why Go:** Single static binary, trivial VPS deployment, excellent TLS and HTTP stdlib.

---

### How It Works

```
Mac OS 9 (plain HTTP) → MacSurf Proxy (Go) → Internet (HTTPS) → Proxy → Mac
```

The proxy:
1. Listens on a configurable port (default 8765)
2. Receives standard HTTP proxy requests (`CONNECT` for tunneling, `GET` for standard)
3. For HTTP: forwards as-is
4. For HTTPS: establishes TLS connection to destination, relays traffic
5. Returns response as plain HTTP to the Mac

---

### Features

- Single binary, no install — just run it
- Optional basic auth (username/password) to prevent open relay abuse
- Domain allowlist/blocklist (optional, for self-hosted installs)
- Request logging (optional, off by default for privacy)
- HTTPS to destination — the proxy speaks modern TLS so the Mac doesn't have to
- HTTP/1.1 support, keep-alive

---

### Deployment Options

**Option A: Local (same network)**
Run on any modern machine on your LAN. Mac points to that machine's IP.
```bash
./macsurf-proxy --port 8765
```

**Option B: VPS (public)**
Deploy to any cheap VPS (Hetzner, BuyVM, etc.). Add basic auth.
```bash
./macsurf-proxy --port 8765 --auth user:password
```

**Option C: Docker**
```dockerfile
FROM scratch
COPY macsurf-proxy /macsurf-proxy
EXPOSE 8765
ENTRYPOINT ["/macsurf-proxy"]
```

---

### Proxy API (Internal)

The proxy speaks standard HTTP proxy protocol — no custom protocol needed. Classilla already supports HTTP proxies, so MacSurf Browser gets this for free too. The proxy just needs to be better at it than the generic options.

---

## Stretch Goals

- **MacSurf Proxy as a Mac OS 9 app** — run the proxy locally on the Mac itself using Open Transport, no external machine needed. Hard but possible on a fast G4.
- **Feed reader** — simple RSS/Atom reader built in, since most modern content is inaccessible anyway
- **Download manager** — queue downloads, resume support via Open Transport
- **Certificate viewer** — show HTTPS cert info fetched by proxy, displayed in browser
- **Gopher support** — NetSurf already has it, expose it in the UI

---

## Repository Structure

```
macsurf/
├── browser/              # NetSurf fork with macos9 frontend
│   ├── frontends/
│   │   └── macos9/
│   │       ├── main.c
│   │       ├── window.c
│   │       ├── fetch.c       # Open Transport fetcher
│   │       ├── bitmap.c      # QuickDraw rendering
│   │       ├── font.c        # ATM/QuickDraw text
│   │       ├── menu.c
│   │       ├── prefs.c
│   │       └── proxy.c       # Proxy preference integration
│   └── Makefile.macos9
├── proxy/                # MacSurf Proxy (Go)
│   ├── main.go
│   ├── proxy.go
│   ├── auth.go
│   ├── config.go
│   └── Dockerfile
├── docs/
│   ├── building.md
│   ├── deploying-proxy.md
│   └── user-guide.md
└── README.md
```

---

## Immediate Next Steps

1. Stand up the cross-compilation toolchain and get Hubbub compiling for PPC
2. Get a "hello world" Carbon app building that opens a window
3. Study the RISC OS frontend in detail — map every callback to its Mac Toolbox equivalent
4. Write the MacSurf Proxy in Go and test it against Classilla to validate the approach
5. Implement Open Transport HTTP fetcher as a standalone test before wiring into NetSurf

---

## Prior Art & References

- [NetSurf source](https://git.netsurf-browser.org/netsurf.git) — especially `frontends/riscos/` and `frontends/amiga/`
- [Classilla source](https://github.com/classilla/classilla) — how they solved cooperative multitasking
- [Open Transport docs](https://developer.apple.com/library/archive/documentation/mac/NetworkingInternet/NetworkingInternet-2.html)
- [Carbon API reference](https://developer.apple.com/library/archive/documentation/Carbon/Reference/Carbon_ref/Introduction/Introduction.html)
- [68kMLA forums](https://68kmla.org) — community knowledge base
- [Macintosh Garden](https://macintoshgarden.org) — software reference