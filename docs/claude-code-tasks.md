# MacSurf, Claude Code Task List

Hand these to Claude Code one at a time. Each task is self-contained with clear inputs, outputs, and success criteria.

---

## Stage 1: Research & Setup

### Task 1.1, Clone and audit NetSurf
```
Clone the NetSurf repository from https://git.netsurf-browser.org/netsurf.git
and the following dependency libraries:
- https://git.netsurf-browser.org/libhubbub.git
- https://git.netsurf-browser.org/libcss.git
- https://git.netsurf-browser.org/libdom.git
- https://git.netsurf-browser.org/libparserutils.git
- https://git.netsurf-browser.org/libwapcaplet.git

Then produce a markdown report covering:
1. Which C standard each library targets
2. All POSIX dependencies used (pthread, sockets, file I/O, etc.)
3. All platform-specific #ifdefs already present
4. The complete list of functions in NetSurf's frontend API
   (look in include/netsurf/browser_window.h and include/netsurf/*.h)
5. How the RISC OS frontend is structured (list all files in frontends/riscos/)
6. How the AmigaOS frontend is structured (list all files in frontends/amiga/)

Output: docs/research/netsurf-audit.md
```

### Task 1.2, Map NetSurf frontend API to Mac Toolbox
```
Read docs/research/netsurf-audit.md and the NetSurf frontend header files.
Read the RISC OS frontend source as reference.

Produce a mapping document that lists every function in the NetSurf
frontend API and maps it to the Mac OS 9 Carbon/Toolbox equivalent.

Format as a markdown table with columns:
- NetSurf function
- Purpose
- RISC OS implementation (brief)
- Mac Toolbox equivalent
- Notes / complexity

Flag any functions that have no direct Mac equivalent and need
custom solutions.

Output: docs/research/frontend-api-mapping.md
```

### Task 1.3, Audit POSIX dependencies for portability
```
Read docs/research/netsurf-audit.md.

For each POSIX dependency identified, determine:
1. Does Classic Mac OS 9 / Carbon have an equivalent?
2. Can it be stubbed/removed without breaking core functionality?
3. What is the replacement strategy?

Produce a portability report.

Output: docs/research/posix-portability.md
```

### Task 1.4, Set up PPC cross-compilation toolchain
```
Set up a cross-compilation toolchain on Linux that can compile C code
targeting PowerPC (big-endian) as used in Mac OS 9 era machines.

Use gcc-powerpc-linux-gnu or equivalent.

Write a minimal hello world C program, compile it for PPC, and verify
it produces a PPC ELF binary using `file` and `objdump`.

Document the exact toolchain setup steps.

Output:
- tools/hello-ppc.c
- docs/research/toolchain-setup.md
```

### Task 1.5, Compile NetSurf dependency libraries for PPC
```
Using the toolchain from Task 1.4, attempt to compile each dependency
library for PowerPC:
- libparserutils
- libwapcaplet
- libhubbub
- libdom
- libcss

For each library, document:
- Whether it compiled cleanly
- Any errors encountered
- Any source changes required to fix errors
- Final status (success / partial / blocked)

Output: docs/research/ppc-compile-results.md
```

---

## Stage 2: Proxy First

> Build the proxy before the browser. It's simpler, immediately useful,
> and validates the core concept with Classilla today.

### Task 2.1, Scaffold MacSurf Proxy in Go
```
Create the initial Go project structure for the MacSurf Proxy.

Requirements:
- Single binary, no external dependencies (stdlib only)
- Listens on configurable port (default 8765)
- Handles standard HTTP proxy protocol (GET and CONNECT methods)
- For plain HTTP requests: forward as-is
- For CONNECT (HTTPS tunneling): establish TLS to destination,
  relay traffic, return as plain HTTP to client
- Optional basic auth via --auth user:password flag
- Optional --port flag
- Graceful shutdown on SIGINT

Files to create:
- proxy/main.go
- proxy/proxy.go
- proxy/auth.go
- proxy/README.md

Success criteria: can configure Classilla or curl to use it as a proxy
and successfully fetch https://example.com
```

### Task 2.2, Test proxy against curl and document
```
Using the proxy from Task 2.1:

1. Start the proxy on port 8765
2. Test with curl:
   curl -x http://localhost:8765 https://example.com
   curl -x http://localhost:8765 https://macintoshgarden.org
3. Test with basic auth enabled
4. Document any issues found and fix them

Output: docs/proxy-test-results.md
```

### Task 2.3, Dockerfile and VPS deployment guide
```
Write a Dockerfile for the MacSurf Proxy that:
- Produces a minimal single-binary image (use scratch or alpine base)
- Exposes port 8765
- Accepts --auth and --port as environment variables

Also write a deployment guide covering:
- Building and running with Docker
- Running as a bare binary on Ubuntu/Debian VPS
- Setting up as a systemd service
- Basic security recommendations (firewall, auth)

Output:
- proxy/Dockerfile
- docs/deploying-proxy.md
```

---

## Stage 3: Browser Skeleton

### Task 3.1, Create macos9 frontend scaffold
```
In the NetSurf source tree, create the scaffold for the macos9 frontend
modeled on frontends/riscos/.

Create empty stub files with the correct function signatures for every
frontend API function identified in docs/research/frontend-api-mapping.md.
Every stub should compile and return a sensible default (NULL, false, etc.)

Files to create in frontends/macos9/:
- main.c          (entry point, event loop)
- window.c        (window creation and management)
- bitmap.c        (image/bitmap handling)
- font.c          (text measurement and drawing)
- fetch.c         (network fetching via Open Transport)
- menu.c          (menu bar)
- prefs.c         (preferences dialog)
- proxy.c         (proxy configuration)
- Makefile.macos9

Success criteria: the scaffold compiles cleanly against the NetSurf
core with no errors (warnings OK).
```

### Task 3.2, Implement WaitNextEvent scheduler
```
In frontends/macos9/main.c, implement the cooperative multitasking
scheduler.

Requirements:
- Main loop calls WaitNextEvent with a short timeout (1-5ms)
- Dispatches Mac events (mouseDown, keyDown, updateEvt, etc.) to handlers
- Also runs a queue of deferred work items (fetch chunks, layout passes)
  each iteration — each work item does a small unit of work and yields
- Work items are added to the queue by the fetch and layout systems
- Loop continues until quit flag is set

This is the foundation everything else builds on. Get this right before
moving to other tasks.

Reference: how Classilla implements its event loop (study classilla source)

Output: updated frontends/macos9/main.c with documented scheduler
```

---

## Stage 4: Iterate from here

> Once Stage 3 is done, tasks will be generated for each frontend
> subsystem: window.c, bitmap.c, font.c, fetch.c in that order.
> Each builds on the last. Do not start Stage 4 until the scheduler
> in Task 3.2 is solid.

---

## Notes for Claude Code

- Always read CLAUDE.md before starting any task
- Read the reference frontend source before writing any new code
- If a task is blocked, document why in the output file and stop ,
  don't invent solutions to blockers without flagging them
- Prefer small focused commits over large changes
- No external Go dependencies in the proxy, stdlib only
