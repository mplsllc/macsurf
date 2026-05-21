# MacSurf JavaScript Implementation Plan
# Duktape on Mac OS 9 PowerPC, Full Implementation Guide

---

## Why This Is Historically Significant

No consumer browser has run a modern ECMAScript engine on Mac OS 9 PowerPC hardware.
Classic IE 5.x shipped with Jscript 5 but it predates ES5. This would be the first
ES5-capable JavaScript engine running on Mac OS 9, on real hardware from 1998-2000.

---

## Engine Choice: Duktape 2.x

Duktape is the only viable candidate. Reasons:

- **C89 clean**, compiles with CodeWarrior 8's strict C89 mode
- **PPC big-endian supported**, `DUK_USE_BYTEORDER=3` is a first-class config
- **Single-file amalgamation**, `duktape.c` + `duktape.h` + `duk_config.h`, no build system required
- **Runs on 32KB RAM minimum**, 192MB on the G4 is effectively unlimited for our purposes
- **~160-200KB code footprint**, fits comfortably in the CW8 build
- **ES5 full support**, partial ES6, covers virtually all pre-2015 web code
- **NetSurf already has js_thread stubs**, the wiring points exist, they just need to be activated
- **MIT licensed**, compatible with MPLS License

Do NOT use QuickJS, it requires a modern C compiler (C99+). Do NOT attempt to port V8 or SpiderMonkey.

---

## What Duktape Gives You on Mac OS 9

Full ES5.1:
- DOM manipulation via Duktape/C bindings we write
- Event handlers (onclick, onsubmit, onload, onerror)
- XMLHttpRequest stub (routes through our OT fetcher)
- setTimeout/setInterval (cooperative, not threaded)
- JSON.parse / JSON.stringify
- Array, Object, String, Math, full ES5 builtins
- Regular expressions

Not available without significant additional work:
- ES6 Promises (partial, polyfillable)
- Fetch API (needs custom binding)
- WebSockets (OT can do TCP, binding needed)
- CSS animations triggered by JS (possible but slow)
- Canvas (possible but extremely slow on QuickDraw)

---

## Hardware Constraints

**Target:** Gary's G4 (Sonnet upgrade, 400MHz), 192MB RAM, Mac OS 9.1

**Memory budget:**
- NetSurf + all libraries: ~8-12MB estimated
- Duktape heap per page: 2-8MB typical for real sites
- DOM tree for a typical page: 1-4MB
- Total per-tab JS: 4-12MB
- Remaining for OS + other: ~170MB

**Conclusion:** Memory is not a constraint. A 400MHz G4 is roughly equivalent
to a 200MHz Pentium II in JS performance, slow by modern standards but fast
enough for pre-2010 web code. Google Maps circa 2006 probably won't work.
Wikipedia, GitHub (old), Hacker News, Reddit old, most forms, yes.

**Stack depth:** Mac OS 9 cooperative threads have limited stack. Set
`DUK_USE_NATIVE_CALL_RECLIMIT` conservatively (128 or 256). Deep recursive
JS will hit this but normal site JS won't.

---

## Phase 0: duk_config.h for CW8/Mac OS 9 PPC

This is the most critical step. Duktape's configure.py generates duk_config.h
for known platforms. Mac OS 9 + CW8 is not a known platform, so we hand-craft it.

### Required defines

```c
/* Platform */
#define DUK_USE_OS_STRING "macos9"
#define DUK_USE_ARCH_STRING "ppc32"
#define DUK_USE_COMPILER_STRING "codewarrior8"

/* PowerPC big-endian, both integers and IEEE doubles */
#define DUK_USE_BYTEORDER 3

/* 32-bit pointers — enables packed 8-byte duk_tval (faster, smaller) */
#define DUK_USE_PACKED_TVAL

/* Alignment — PPC requires 4-byte alignment minimum, use 8 for doubles */
#define DUK_USE_ALIGN_BY 8

/* No POSIX threads — we use cooperative multitasking */
#undef DUK_USE_PTHREAD_API

/* Date provider — use Mac OS 9 GetDateTime() */
#define DUK_USE_DATE_NOW_MACOS9
/* We implement duk_bi_date_get_now_macos9() in macos9_date.c */

/* No longjmp via C++ exceptions — use C setjmp/longjmp */
#define DUK_USE_SETJMP

/* Disable features that pull in large libc code */
#undef DUK_USE_FILE_IO        /* no fopen etc */
#define DUK_USE_PROVIDE_DEFAULT_ALLOC  /* use malloc/free from MSL */

/* Memory: reference counting + mark-and-sweep (default, recommended) */
#define DUK_USE_REFERENCE_COUNTING
#define DUK_USE_MARK_AND_SWEEP

/* Recursion limits — conservative for limited Mac OS 9 stack */
#define DUK_USE_NATIVE_CALL_RECLIMIT 128
#define DUK_USE_ECMASCRIPT_CALL_RECLIMIT 256

/* Fatal error handler — we provide our own */
#define DUK_USE_FATAL_HANDLER(udata, msg) macsurf_js_fatal(udata, msg)

/* Disable regexp if size is a concern — enable for real sites */
#define DUK_USE_REGEXP_SUPPORT

/* Disable debug output in release builds */
#undef DUK_USE_DEBUG
```

### CW8-specific patches needed in duktape.c

Duktape 2.x is C89 clean but has a few patterns CW8 may reject:

1. **`volatile` in unexpected places**, CW8 is strict about volatile qualifiers
2. **`__FILE__` and `__LINE__`**, fine, CW8 supports these
3. **`va_list` / `va_copy`**, CW8's MSL defines `va_copy`; add `#ifndef va_copy` guard
4. **`double` promotion rules**, CW8 may warn on implicit float/double mixing
5. **`setjmp.h`**, available in MSL, should work as-is
6. **`<string.h>`** naming collision, same issue we hit with NetSurf; prefix file
   must ensure MSL's string.h is found, not any internal one

The agent should run `duktape.c` through the Linux C89 syntax check before
touching CW8. Most issues will be visible there.

---

## Phase 1: Port Duktape to the Build

### Step 1.1, Get Duktape 2.7.0 source

```bash
# On Hetzner
wget https://duktape.org/duktape-2.7.0.tar.xz
tar xf duktape-2.7.0.tar.xz
cp duktape-2.7.0/src/duktape.c browser/libduktape/
cp duktape-2.7.0/src/duktape.h browser/libduktape/
cp duktape-2.7.0/src/duk_config.h browser/libduktape/
```

### Step 1.2, Create hand-crafted duk_config.h

Copy the base from `duktape-2.7.0/src/duk_config.h` and apply the Mac OS 9
overrides described in Phase 0. Save as `browser/libduktape/duk_config.h`.

### Step 1.3, Linux C89 audit of duktape.c

```bash
gcc -std=c89 -pedantic -Wall -Wno-long-long \
    -DDUK_COMPILING_DUKTAPE \
    -I browser/libduktape \
    -c browser/libduktape/duktape.c -o /dev/null 2>&1 | head -100
```

Fix all hard errors. Warnings about `long long` are acceptable (we allow it
in the prefix file via `#pragma`).

### Step 1.4, Add to CW8 project

Add `duktape.c` to the file list. Add `browser:libduktape:` to access paths.
`duktape.h` and `duk_config.h` are headers only, do not add to file list.

---

## Phase 2: Platform Bindings

Duktape needs a few platform hooks we must implement for Mac OS 9.

### macos9_date.c, Date provider

```c
/* Called by Duktape for Date.now() */
duk_double_t macsurf_js_get_now(void) {
    unsigned long secs;
    GetDateTime(&secs);
    /* Mac epoch is Jan 1 1904; Unix epoch is Jan 1 1970 */
    /* Difference: 2082844800 seconds */
    return (duk_double_t)(secs - 2082844800UL) * 1000.0;
}
```

### macos9_js_fatal.c, Fatal error handler

```c
void macsurf_js_fatal(void *udata, const char *msg) {
    /* Log to debug console, then recover gracefully */
    DebugStr("\pJS fatal error");
    /* Do NOT call exit() or abort() — we want to recover */
    /* Destroy the heap and report error to the page */
    longjmp(macsurf_js_recovery_jmp, 1);
}
```

### Memory allocator

Use MSL's `malloc`/`realloc`/`free` directly. No custom allocator needed at
192MB RAM. If memory becomes an issue later, Duktape's pool allocator
(`extras/alloc-pool/`) can be dropped in.

---

## Phase 3: Remove WITHOUT_DUKTAPE Gate

The NetSurf HTML handler already has `js_thread` infrastructure stubbed out
behind `#ifndef WITHOUT_DUKTAPE`. The js_stub.c file provides no-op stubs.

Steps:
1. Remove `#define WITHOUT_DUKTAPE` from `macsurf_prefix.h`
2. Add `#include "duktape.h"` to `browser/netsurf/content/handlers/html/js.h`
3. Implement `browser/netsurf/frontends/macos9/javascript/macsurf_js.c` ,
   this is the glue between NetSurf's js_thread API and Duktape

### macsurf_js.c skeleton

```c
#include "duktape.h"
#include "content/handlers/html/js.h"

struct jscontext {
    duk_context *duk;
};

struct jscontext *js_newcontext(int timeout, jsobjtype window, void **private) {
    struct jscontext *ctx;
    ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->duk = duk_create_heap_default();
    if (!ctx->duk) { free(ctx); return NULL; }
    macsurf_js_setup_globals(ctx->duk);  /* register DOM bindings */
    return ctx;
}

void js_destroycontext(struct jscontext *ctx) {
    if (ctx && ctx->duk) {
        duk_destroy_heap(ctx->duk);
    }
    free(ctx);
}

bool js_exec(struct jscontext *ctx, const char *src, size_t srclen) {
    if (!ctx || !ctx->duk) return false;
    if (duk_peval_lstring(ctx->duk, src, srclen) != 0) {
        /* Script error — log it, don't crash */
        return false;
    }
    duk_pop(ctx->duk);
    return true;
}
```

---

## Phase 4: DOM Bindings

This is the largest phase. DOM bindings are Duktape/C functions that bridge
JS calls to NetSurf's libdom.

### Priority order (implement these first)

1. **`document.getElementById(id)`**, needed by almost everything
2. **`element.innerHTML` setter**, needed for dynamic content
3. **`element.style` setter**, CSS manipulation
4. **`element.addEventListener(event, fn)`**, event handling
5. **`document.createElement(tag)`**, dynamic DOM
6. **`element.appendChild(node)`**, DOM insertion
7. **`window.location.href`**, navigation
8. **`XMLHttpRequest`**, AJAX (routes through OT fetcher)
9. **`document.querySelector(selector)`**, modern sites use this heavily
10. **`console.log()`**, debugging, logs to MacSurf debug window

### Binding pattern

```c
/* Example: document.getElementById */
static duk_ret_t macsurf_getElementById(duk_context *duk) {
    const char *id = duk_require_string(duk, 0);
    dom_element *el = NULL;
    dom_string *id_str;

    dom_string_create((const uint8_t *)id, strlen(id), &id_str);
    dom_document_get_element_by_id(current_document, id_str, &el);
    dom_string_unref(id_str);

    if (!el) {
        duk_push_null(duk);
        return 1;
    }

    /* Push a JS object that wraps the dom_element pointer */
    macsurf_push_element(duk, el);
    return 1;
}
```

### JS object / C pointer mapping

The trickiest part. We use Duktape's `duk_push_pointer` / `duk_get_pointer`
to store `dom_node *` pointers inside JS objects, with a finalizer to
`dom_node_unref` when the JS object is garbage collected.

---

## Phase 5: Event Loop Integration

Mac OS 9 is cooperative multitasking. Duktape has no threading. We need a
minimal event loop that:

1. Calls `WaitNextEvent` for Mac UI events
2. Calls pending JS setTimeout/setInterval callbacks
3. Processes any queued network responses (OT)
4. Calls `duk_gc` periodically to reclaim memory

```c
/* In main.c event loop, after WaitNextEvent */
void macsurf_js_pump(struct jscontext *ctx) {
    macsurf_js_run_timers(ctx);  /* fire due setTimeout/setInterval */
    macsurf_js_run_events(ctx);  /* fire queued DOM events */
}
```

setTimeout is implemented as a sorted list of `{expiry_time, duk_function_ref}`
pairs checked each iteration of the event loop.

---

## Phase 6: XMLHttpRequest

XHR is the key to making modern sites functional. Route all XHR through
the existing OT fetcher:

1. JS calls `new XMLHttpRequest()`
2. Duktape/C creates an XHR object wrapping a `macsurf_fetch_ctx`
3. `.open(method, url)` sets up the fetch parameters
4. `.send(body)` queues an async OT request
5. On response, NetSurf's fetch callback fires `onreadystatechange`

The proxy on Hetzner already strips TLS, so XHR to HTTPS URLs will work
via the proxy transparently.

---

## Known Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| CW8 rejects duktape.c patterns | Medium | C89 audit pass before Mac; most issues fixable |
| Stack overflow in deep JS recursion | High for complex sites | Set reclimit to 128; most sites won't hit it |
| OOM on large JS-heavy pages | Low at 192MB | Monitor with MacsBug; add heap size cap |
| `longjmp` interaction with CW8 C++ exception model | Medium | Use C-only setjmp, no C++ in Duktape path |
| `double` precision on PPC | Low | PPC has full IEEE 754; no issues expected |
| `va_copy` conflict with MSL | Known | Already solved, `#ifndef va_copy` guard |
| Performance too slow for modern JS | High for heavy sites | Expected; focus on pre-2015 sites |
| DOM binding crashes on malformed pages | Medium | Null-check everything; wrap in `duk_safe_call` |

---

## Test Plan

### Milestone 1: Engine compiles and runs
- `duktape.c` compiles cleanly in CW8
- `duk_eval_string(ctx, "1+1")` returns 2
- `duk_eval_string(ctx, "typeof window")` returns `"object"`

### Milestone 2: Basic DOM works
- `document.title` returns the page title
- `document.getElementById("foo")` finds an element
- `element.innerHTML = "hello"` updates the page visually

### Milestone 3: Events work
- `<button onclick="alert('hi')">` shows an alert dialog
- `addEventListener` fires on click

### Milestone 4: Real sites
- Hacker News, upvote, collapse threads
- Wikipedia, search autocomplete
- Old Reddit, vote, expand comments
- DuckDuckGo, search form submission

---

## File Structure

```
browser/
  libduktape/
    duktape.c           — amalgamated engine (DO NOT EDIT)
    duktape.h           — public API header
    duk_config.h        — Mac OS 9 PPC configuration (hand-crafted)
  netsurf/
    frontends/
      macos9/
        javascript/
          macsurf_js.c          — js_thread API implementation
          macsurf_js_dom.c      — DOM bindings (getElementById etc)
          macsurf_js_xhr.c      — XMLHttpRequest binding
          macsurf_js_timers.c   — setTimeout/setInterval
          macsurf_js_date.c     — Date.now() provider
          macsurf_js_fatal.c    — fatal error handler
          macsurf_js.h          — internal header
```

---

## Agent Prompt for Phase 0+1

```
Read CLAUDE.md before starting.

Begin MacSurf JavaScript implementation — Phase 0 and Phase 1.

1. Download Duktape 2.7.0 from https://duktape.org/duktape-2.7.0.tar.xz
   and copy src/duktape.c, src/duktape.h, src/duk_config.h to browser/libduktape/.

2. Create a hand-crafted duk_config.h for Mac OS 9 PPC + CodeWarrior 8
   based on the spec in docs/macsurf-javascript-plan.md Phase 0. Key settings:
   DUK_USE_BYTEORDER=3 (big-endian PPC), DUK_USE_PACKED_TVAL, DUK_USE_ALIGN_BY=8,
   DUK_USE_SETJMP, DUK_USE_NATIVE_CALL_RECLIMIT=128.

3. Run the Linux C89 audit on duktape.c:
   gcc -std=c89 -pedantic -Wall -DDUK_COMPILING_DUKTAPE -I browser/libduktape \
       -c browser/libduktape/duktape.c -o /dev/null 2>&1
   Fix all hard errors. Document any warnings.

4. Create browser/libduktape/ directory structure and commit.

5. Transfer duktape.c, duktape.h, duk_config.h to laptop as macsurf-duktape-port.zip.

Report back with C89 audit results and any fixes needed.
```