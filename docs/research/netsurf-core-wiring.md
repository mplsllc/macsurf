# NetSurf Core Wiring Audit (HTML + CSS Content Handlers + HTTP Fetcher)

## Status

**Research only, no code changes.** This document audits what's required
to replace the stub implementations of `html_init`, `nscss_init`,
`image_init`, `fetcher_init`, and `hlcache_initialise` with real calls
into the now-ported NetSurf libraries (libparserutils, libhubbub,
libdom, libcss).

## What this milestone is

The four library ports are done; libparserutils, libhubbub, libdom, and
libcss all build C89-clean and are wired into `MacSurf.mcp` (see
[parserutils-port.md](parserutils-port.md), [libhubbub-port.md](libhubbub-port.md),
[libdom-port.md](libdom-port.md), [libcss-port.md](libcss-port.md)).
What's still stubbed is the **glue layer** between NetSurf core and
those libraries: the html and css content handlers, the HTTP fetcher,
and the corresponding `*_init()` calls invoked by `netsurf_init()`.

The end goal is the v0.1.0 pipeline replaced by the real one:

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

## 1. The HTML content handler

**Location:** [`browser/netsurf/content/handlers/html/`](../../browser/netsurf/content/handlers/html/)

**File counts:** 23 `.c` files, 21 `.h` files, **32,079 LOC** ,
comparable in size to libdom.

### File list

```
box_construct.c        box_construct.h
box.h                  (header only — defines struct box)
box_inspect.c          box_inspect.h
box_manipulate.c       box_manipulate.h
box_normalise.c        box_normalise.h
box_special.c          box_special.h
box_textarea.c         box_textarea.h
css.c                  css.h           (the html-side CSS bridge — distinct from content/handlers/css/)
css_fetcher.c          (the file:// fetcher for embedded stylesheets)
dom_event.c            dom_event.h
font.c                 font.h
form.c                 form_internal.h
forms.c                (a separate file from form.c)
html.c                 html.h          (the entry point — defines html_init)
html_save.h
imagemap.c             imagemap.h
interaction.c          interaction.h
layout.c               layout.h        (the heart of the layout engine)
layout_flex.c          (flexbox-specific layout)
layout_internal.h
object.c               object.h        (image/object embedding)
private.h
redraw.c               redraw_border.c
script.c               (JS hook — stubs out to nothing without WITH_DUKTAPE)
table.c                table.h
textselection.c        textselection.h
```

### `html_init()` body

From [`html.c:1962`](../../browser/netsurf/content/handlers/html/html.c#L1962):

```c
nserror html_init(void)
{
    uint32_t i;
    nserror error;

    error = html_css_init();
    if (error != NSERROR_OK)
        goto error;

    for (i = 0; i < NOF_ELEMENTS(html_types); i++) {
        error = content_factory_register_handler(html_types[i],
                &html_content_handler);
        if (error != NSERROR_OK)
            goto error;
    }
    return NSERROR_OK;
}
```

So `html_init`:
1. Calls `html_css_init()` (in `html/css.c`), initializes the CSS bridge for HTML
2. Registers the `html_content_handler` for each MIME type in `html_types[]` (text/html, application/xhtml+xml, etc.) via `content_factory_register_handler`

For this to work, two prerequisites must be true:
- **`content_factory_register_handler` must be available**, i.e.
  `content/content_factory.c` must be in the build (it is NOT today)
- **The `html_content_handler` struct** at `html.c:2345-2360` must be a
  valid `content_handler` and all its function pointer targets
  (`html_create`, `html_process_data`, `html_convert`, `html_reformat`,
  `html_destroy`, etc.) must compile and link

## 2. The CSS content handler

**Location:** [`browser/netsurf/content/handlers/css/`](../../browser/netsurf/content/handlers/css/)

**File counts:** 5 `.c` files, 6 `.h` files, **6,590 LOC**, much
smaller than the html handler.

### File list

```
css.c     css.h        (the entry point — defines nscss_init)
dump.c    dump.h       (debug dumping; can probably stub)
hints.c   hints.h      (HTML presentational hints → CSS)
internal.c internal.h  (internal CSS state)
select.c  select.h     (selector matching glue)
utils.h
```

### `nscss_init()` body

From [`css.c:814`](../../browser/netsurf/content/handlers/css/css.c#L814):

```c
nserror nscss_init(void)
{
    nserror error;

    error = content_factory_register_handler("text/css",
            &css_content_handler);
    if (error != NSERROR_OK)
        goto error;

    error = css_hint_init();
    if (error != NSERROR_OK)
        goto error;

    return NSERROR_OK;
}
```

Same shape as `html_init`: register the css content handler, then call
`css_hint_init()` (in `hints.c`) to set up the presentational-hint
mapping. Same prerequisite, needs `content_factory_register_handler`.

## 3. C99 / CW8 audit of HTML + CSS handler files

Same battery as the prior library port audits, run across all 28 files
in `content/handlers/html/` and `content/handlers/css/`:

| Category | Count | Verdict |
|---|---:|---|
| `inline` keyword | **17 files** | Already covered by `#define inline` in prefix |
| `//` line comments | **0 files** | Already clean, no fix needed |
| Designated initializers | **9 files** | Need converting (assignment-statement form) |
| For-scope declarations | **1 file** (`layout_flex.c`) | Need converting to C89 |
| `restrict` keyword | 0 | n/a |
| `long long` | 0 | n/a |
| `__VA_ARGS__` | 0 | n/a |
| `snprintf` | **4 files** | CW8 MSL provides it |
| `%zu` printf | 1 file | Probably gated; check on first build |
| Compound literals | 0 | n/a |
| `<strings.h>` | **7 files** | Already shimmed (libhubbub port) |
| `<math.h>` | **1 file** (`layout_flex.c`) | New, CW8 MSL has it but check |
| `<nsutils/time.h>` | 1 file (`html.c`) | Already stubbed in `frontends/macos9/nsutils/time.h` |

**Designated initializers** (9 files), these need fixing using the
same assignment-statement playbook from libdom and libcss. Sample sites:

```
html/html.c:314-316              .getdims = { .viewport_width = ..., ... }
html/html.c:2345-2360            html_content_handler vtable initializer
                                 (this is the static const that ALL of
                                  html.c hangs off — ~16 function pointer
                                  fields)
```

The `html_content_handler` static const is the most important, it's a
file-scope `static const struct content_handler` initializer with
designated `.field = func` syntax. C89 forbids this. **Same fix as
libcss `format_list_style.c`:** convert to positional initializer in
struct field order via a Python helper.

**For-scope declaration** (1 file), `layout_flex.c` has at least one
`for (size_t i = ...)` site. Trivial fix.

### `<dom/bindings/hubbub/parser.h>`, path issue

Four files in NetSurf core do `#include <dom/bindings/hubbub/parser.h>`:

```
browser/netsurf/utils/libdom.h
browser/netsurf/content/handlers/html/private.h
browser/netsurf/content/handlers/javascript/duktape/Element.bnd
browser/netsurf/desktop/hotlist.c
```

The header lives at [`browser/libdom/bindings/hubbub/parser.h`](../../browser/libdom/bindings/hubbub/parser.h),
NOT at `browser/libdom/include/dom/bindings/hubbub/parser.h`. The
upstream layout assumes the binding gets installed as
`${PREFIX}/include/dom/bindings/...` at install time, but we're using
vendored sources where libdom isn't "installed" anywhere.

**Fix shape (3 options):**

1. **Symlink:** create `browser/libdom/include/dom/bindings -> ../../bindings`.
   Works on Linux, doesn't survive zip-based delivery to the Mac.
2. **Wrapper header:** create
   `browser/libdom/include/dom/bindings/hubbub/parser.h` that just
   `#include`s the real path via the libdom user-search-path. Works
   everywhere but adds a tracked file.
3. **Add `browser/libdom` to the search path** so `dom/bindings/...`
   resolves naturally, but only if the libdom dir contains a `dom/`
   subdir, which it doesn't.

**Recommended:** option 2 (wrapper header). One file, no symlink, works
on both Linux and CW8.

## 4. Existing MacSurf stubs that conflict

### `frontends/macos9/html/html.h` and `html/form_internal.h`

Two files, **87 lines total**. These are stubs of the NetSurf-core HTML
content handler types, they declare `struct html_stylesheet`,
`struct content_html_object`, and a partial `struct form`, used by
the existing build's compilation of `desktop/browser_window.c` etc.
which forward-declare these types.

**Reachability check:** these are at `frontends/macos9/html/`. None of
the user search paths in `MacSurf.mcp` is just `{Project}` itself, so
`<html/html.h>` does NOT find them today. They are orphaned but on
disk.

When the real NetSurf core HTML files are added, they will resolve
`#include "html/html.h"` via `{Project}/../../../` (NetSurf source
root) which gives them the real `content/handlers/html/html.h`. So
the stubs become irrelevant the moment we add the real handler.

**Fix shape:** delete `frontends/macos9/html/html.h` and
`frontends/macos9/html/form_internal.h` as part of the wiring step.
Same playbook as the libdom and libcss stub deletions.

### `frontends/macos9/css/css.h` and `css/utils.h`

```
css/css.h     24 lines    (NetSurf core css/css.h stub)
css/utils.h   115 lines   (NetSurf core css/utils.h stub)
```

Same situation as the `html/` stubs, they shadow what would be the
real `content/handlers/css/css.h` and `content/handlers/css/utils.h`.
But because they live in `frontends/macos9/css/` which is not on the
search path, they're orphaned today.

**Fix shape:** delete both as part of the wiring step.

### Total stub footprint to delete in this milestone

```
frontends/macos9/html/html.h           50 lines
frontends/macos9/html/form_internal.h  37 lines
frontends/macos9/css/css.h             24 lines
frontends/macos9/css/utils.h          115 lines
                                      ---
                                      226 lines
```

Smaller deletion than libdom (1,638) or libcss (2,013), because these
stubs were always thinner, they just typedef'd a few structs to keep
the existing 27-file build linking.

## 5. Cascading dependencies, what else has to come into the build

`html_init` and `nscss_init` aren't standalone. The handlers transitively
include and call into a lot of NetSurf core modules that aren't in the
27-file build today.

### Minimum content/ closure

NEW files needed in `MacSurf.mcp`:

| File | Why |
|---|---|
| `content/content_factory.c` | Both `html_init` and `nscss_init` call `content_factory_register_handler`. **REQUIRED.** |
| `content/textsearch.c` | Included by `html.c` via `content/textsearch.h`. Used for find-in-page. |
| `content/no_backing_store.c` | Alternative to `fs_backing_store.c`, choose one based on whether we want llcache to disk. |

### Minimum utils/ closure

NEW files needed:

| File | Used by |
|---|---|
| `utils/corestrings.c` | `html.c`, `css.c`, virtually every NetSurf core file |
| `utils/libdom.c` | `html.c` (via `utils/libdom.h`), provides libdom helpers like `libdom_iterate_child_elements` |
| `utils/talloc.c` | `html.c`, talloc is a hierarchical memory pool used by the box tree |
| `utils/hashtable.c` | Used by various NetSurf modules indirectly |
| `utils/bloom.c` | Bloom filter used by selector matching |
| `utils/idna.c` | International Domain Name handling, used by URL parsing |
| `utils/punycode.c` | Punycode for IDN |
| `utils/useragent.c` | User-Agent string generation |
| `utils/nscolour.c` | System colour palette |
| `utils/ssl_certs.c` | Cert chain, may be stubbable if we don't do TLS in the browser |

The current 10 utils/ files (`utils.c`, `file.c`, `filepath.c`, `log.c`,
`time.c`, `nsurl.c`, `url.c`, `utf8.c`, `messages.c`, `nsoption.c`)
stay as they are. **9-10 new utils files** to add.

### Minimum desktop/ closure

NEW files needed:

| File | Used by |
|---|---|
| `desktop/selection.c` | `html.c`, text selection state |
| `desktop/scrollbar.c` | `html.c`, scrollable region scroll bars |
| `desktop/textarea.c` | `html.c`, `<textarea>` editing |
| `desktop/system_colour.c` | `css/css.c`, system colour resolution |

Maybe more, depending on what `html_create` and friends pull in. The
current 5 desktop files (`browser.c`, `browser_window.c`, `netsurf.c`,
`gui_factory.c`, `plot_style.c`) stay.

### Image handler, the gated escape hatch

`image_init()` from `content/handlers/image/image.c`:

```c
nserror image_init(void)
{
    nserror error = NSERROR_OK;

#ifdef WITH_BMP
    error = nsbmp_init();
    ...
#endif
#ifdef WITH_GIF
    error = nsgif_init();
    ...
#endif
#ifdef WITH_JPEG
    ...
#endif
    /* etc. */

    return error;
}
```

**The whole image content handler is feature-flagged.** If we don't
define `WITH_BMP`, `WITH_GIF`, `WITH_PNG`, etc., `image_init()` reduces
to `return NSERROR_OK` and **not a single image library is needed**.
The `image.c` file by itself compiles standalone.

**Implication:** image support can be deferred indefinitely, we just
include `image.c` (a no-op stub equivalent) and skip the format-specific
files. v0.2 ships with no image rendering and that's a deliberate
scope decision, not a hack. v0.3 milestone adds libnsgif first
(smallest, simplest format), then libnsbmp, then later libpng/libjpeg.

## 6. The HTTP fetcher, `macos9_http_fetcher.c`

The current [`macos9_fetch.c`](../../browser/netsurf/frontends/macos9/macos9_fetch.c)
implements `macos9_fetch_url()` as a single synchronous function: open
endpoint → bind → connect → send → receive → callback → close. This
is the v0.1.0 pipeline and bypasses NetSurf's fetcher system entirely.

The new file (call it `macos9_http_fetcher.c`) implements a real
`fetcher_operation_table` and registers it via `fetcher_add()` so
NetSurf core's `hlcache_handle_retrieve` can route HTTP fetches
through it.

### `fetcher_operation_table` (verbatim from `content/fetchers.h`)

```c
struct fetcher_operation_table {
    bool   (*initialise)(lwc_string *scheme);
    bool   (*acceptable)(const struct nsurl *url);
    void  *(*setup)(struct fetch *parent_fetch, struct nsurl *url,
                    bool only_2xx, bool downgrade_tls,
                    const char *post_urlenc,
                    const struct fetch_multipart_data *post_multipart,
                    const char **headers);
    bool   (*start)(void *fetch);
    void   (*abort)(void *fetch);
    void   (*free)(void *fetch);
    void   (*poll)(lwc_string *scheme);
    int    (*fdset)(lwc_string *scheme, fd_set *read_set,
                    fd_set *write_set, fd_set *error_set);
    void   (*finalise)(lwc_string *scheme);
};
```

Registered via `fetcher_add(scheme, &ops)`.

### Reference: `content/fetchers/data.c`

The simplest in-tree fetcher example, ~340 lines, handles `data:` URLs.
Implements all 9 callbacks. Pattern (from
[`data.c:324-339`](../../browser/netsurf/content/fetchers/data.c#L324)):

```c
nserror fetch_data_register(void)
{
    lwc_string *scheme = lwc_string_ref(corestring_lwc_data);
    const struct fetcher_operation_table fetcher_ops = {
        fetch_data_initialise,
        fetch_data_can_fetch,
        fetch_data_setup,
        fetch_data_start,
        fetch_data_abort,
        fetch_data_free,
        fetch_data_poll,
        NULL,                       /* no fdset */
        fetch_data_finalise
    };
    return fetcher_add(scheme, &fetcher_ops);
}
```

Note `data.c` uses **designated initializers** in the original
upstream, they need positional conversion for CW8 in our build (same
treatment as the other ports).

### How the existing `macos9_fetch.c` maps onto fetcher_operation_table

Current code is a single 250-line `macos9_fetch_url()` function. To
become a proper fetcher backend, the OT primitives need to be split
across the 9 callbacks. The OT calls themselves don't change, only
how they're sequenced and where they live.

| Callback | What it does | Reused from current code |
|---|---|---|
| `initialise(scheme)` | Called once per scheme. Verify `corestring_lwc_http` matches. Return true. | (new, 5 lines) |
| `acceptable(url)` | Return `nsurl_get_scheme_type(url) == NSURL_SCHEME_HTTP` (or similar). | (new, 3 lines) |
| `setup(parent, url, ...)` | Allocate a `macos9_fetch_ctx` struct, store url + headers + state machine state. Return ctx pointer. | (new, ~40 lines, state struct definition) |
| `start(ctx)` | Add ctx to a per-scheme ring (linked list of active fetches). Return true. | (new, ~10 lines, ring management) |
| `poll(scheme)` | The work loop. For each fetch in the ring: <br/>• If state is INIT: open endpoint, bind, install yield notifier, switch to async, start OTConnect. State → CONNECTING. <br/>• If state is CONNECTING and notifier saw T_CONNECT: build HTTP request, OTSnd, state → SENDING. <br/>• If state is SENDING and OTSnd done: state → RECEIVING. <br/>• If state is RECEIVING: OTRcv non-blocking; for each byte chunk, parse headers if not yet seen, then call `fetch_send_callback(FETCH_DATA, ...)`. If kOTNoDataErr, return and try next poll. If done, send `FETCH_FINISHED` and remove from ring. | **all the existing OT primitives** from macos9_fetch.c, just split across states |
| `abort(ctx)` | Set abort flag. The next `poll` cleans up. | (new, 5 lines) |
| `free(ctx)` | Close endpoint, dispose UPP, free ctx struct. | reuse current cleanup code |
| `fdset(...)` | Doesn't apply to OT. Return -1 (no fd). | (new, 1 line) |
| `finalise(scheme)` | Per-scheme cleanup. No-op. | (new, 1 line) |

### State machine

The new fetcher needs explicit state. The current code blocks
synchronously through CONNECT/SEND/RECV in one shot. The fetcher_op
contract is poll-based, `poll()` is called periodically by NetSurf
core's event loop and is expected to make progress, not block.

Two ways to implement this:

**Option A: True async via OT notifier**

Use `OTSetAsynchronous(ep)` and a notifier that posts T_CONNECT,
T_DATA, T_DISCONNECT events to a per-fetch state. `poll()` checks the
state and emits FETCH_HEADER / FETCH_DATA / FETCH_FINISHED. This is
the canonical OT pattern from the Classilla LDAP `tcp.c` reference and
is what the audit's first OT research said to use.

**Option B: Sync OT with `OTUseSyncIdleEvents` + cooperative thread**

Same approach as the current `macos9_fetch.c`, use sync+blocking
OT calls but install a notifier that calls `YieldToAnyThread()` on
`kOTSyncIdleEvent`, so the cooperative event loop keeps running during
blocked calls. Simpler to implement than async, matches the current
working code.

**Recommendation:** Option B for v0.2. Reuses the existing OT code
verbatim and matches what we know works on real OS 9 hardware
(verified against frogfind.com at v0.1.0). Trade-off is that one slow
fetch can stall other fetches, acceptable for a browser that does
one page load at a time. Option A is the proper Carbon Event Manager
pattern but is more complex and doesn't buy anything for our use case.

### What `macos9_http_fetcher.c` looks like in scope

Estimated size: **~350-450 lines** (similar to `data.c`'s 340 lines
plus the OT primitives we already have). Mostly:
- `struct macos9_fetch_ctx` definition (~30 lines)
- 9 callback functions (~30 lines each = ~270 lines)
- Ring/list management for active fetches (~50 lines)
- The OT primitives (already exist in macos9_fetch.c, just relocated)
- A `macos9_http_fetcher_register()` exported entry point that's
  called from a new `fetcher_init` real implementation in
  `misc_stub.c` or wherever (currently a stub returning OK)

### What replaces `fetch_stub.c`

`fetch_stub.c` provides 9 functions today:
```
fetch_start, fetch_can_fetch, fetch_http_code, fetch_quit, fetcher_quit,
fetch_multipart_data_new_kv, fetch_multipart_data_find,
fetch_multipart_data_clone, fetch_multipart_data_destroy
```

**These are NOT replaced by `macos9_http_fetcher.c`**, they live in
NetSurf core's `content/fetch.c` which is **already in the build**.
The stub conflicts with the real implementation today; once we delete
`fetch_stub.c` from disk and let `content/fetch.c` provide them, the
real `fetch_start` etc. get linked. The real `fetch_start` then routes
to whatever fetcher is registered for the URL's scheme, which is
where our `macos9_http_fetcher` plugs in.

So the action is: **delete `fetch_stub.c`** and rely on the existing
`content/fetch.c` (already in MacSurf.mcp) providing the symbols. This
is parallel to how the prior ports deleted `corestrings_stub.c`/`lwc_stub.c`
mismatches.

**Same with `misc_stub.c::fetcher_init()`**, that stub returns OK
without doing anything. Replace with a real `fetcher_init()` that
calls our `macos9_http_fetcher_register()`. Either delete the stub
function from `misc_stub.c` and let `content/fetch.c::fetcher_init`
take over (it iterates registered fetchers but also tries to register
data/file/about/curl fetchers, those need to be either ported or
stubbed), or write a thin MacSurf-specific replacement.

Cleanest: write a new `macos9_fetcher_init.c` that calls
`macos9_http_fetcher_register()` and nothing else, then export it as
`fetcher_init` and delete the stub from `misc_stub.c`. The result is
a 1-fetcher build (HTTP only via OT). All other URL schemes return
"no fetcher" errors, which is fine for v0.2.

## 7. `hlcache_initialise` and `llcache_initialise`

Already in the build (`content/hlcache.c` and `content/llcache.c` are
both in MacSurf.mcp). The stub at `misc_stub.c::ns_system_colour_init`
and others fire BEFORE these init calls, they're prerequisite
initializers that must succeed for hlcache to start.

`hlcache_initialise(params)` takes a parameters struct with backing
store config. The simplest path:

```c
struct hlcache_parameters hl_params;
struct llcache_store_parameters store_params;
memset(&hl_params, 0, sizeof(hl_params));
memset(&store_params, 0, sizeof(store_params));
hl_params.bg_clean_time = 5000;
hl_params.cb = NULL;
hl_params.cb_ctx = NULL;
hl_params.llcache.minimum_lifetime = 2;
hl_params.llcache.minimum_bandwidth = 32 * 1024;
hl_params.llcache.maximum_bandwidth = 1024 * 1024;
hl_params.llcache.time_quantum = 100;
hl_params.llcache.fetch_attempts = 3;
hl_params.llcache.cb = NULL;
hl_params.llcache.cb_ctx = NULL;
/* No backing store — set the cb pointers to NULL or use no_backing_store.c */
hlcache_initialise(&hl_params);
```

**The current `desktop/netsurf.c::netsurf_init()` calls
`hlcache_initialise` itself** with a params struct it builds from the
options. We don't need to call it from frontend code, `netsurf_init`
already does it. The thing we need to ensure is that the **misc_stub
overrides for `image_cache_init`, `nscolour_update`, `dom_namespace_initialise`,
etc. don't blow up.** They currently return OK without doing anything,
which means the corresponding subsystems are never initialized. Some
of those subsystems are no-ops without HTML/CSS, so the stubs were
fine. Once html_init/nscss_init are real, those stub overrides need to
either:
- Stay as stubs, because the real subsystem is broken without the
  full feature set, OR
- Be replaced with a delegation to the real implementation in
  `content/handlers/.../foo.c`.

Specific stubs that need to become real:

```
misc_stub.c::image_cache_init    → call content/handlers/image/image_cache.c::image_cache_init
misc_stub.c::html_init           → call content/handlers/html/html.c::html_init (DELETE stub, link to real)
misc_stub.c::nscss_init          → call content/handlers/css/css.c::nscss_init   (DELETE stub, link to real)
misc_stub.c::image_init          → call content/handlers/image/image.c::image_init (no-op without WITH_*)
misc_stub.c::textplain_init      → call content/handlers/text/textplain.c (probably stubbable if we don't load text/plain pages)
misc_stub.c::dom_namespace_initialise → call libdom's real one (in libdom/src/...) — already linked, just delete stub
misc_stub.c::nscolour_update      → call utils/nscolour.c (need to add to build)
misc_stub.c::ns_system_colour_init → call desktop/system_colour.c (need to add to build)
misc_stub.c::page_info_init      → call desktop/page-info.c (probably ignorable)
misc_stub.c::fetcher_init        → call new macos9_fetcher_init
misc_stub.c::cert_chain_*        → leave stubs (we don't do TLS)
misc_stub.c::download_context_*  → leave stubs (no downloads)
misc_stub.c::search_web_init     → leave stub
```

The pattern: **for each real implementation we add to the build, the
corresponding stub function in misc_stub.c needs to be removed** so
the linker picks up the real symbol from the new file instead of our
stub.

## 8. What is NOT a problem

- No new POSIX dependencies beyond `<strings.h>` (already shimmed)
- No iconv, no errno, no unistd, no sys/types
- No `__attribute__`, no compound literals, no flex arrays, no VLAs,
  no `__VA_ARGS__`, no `long long`
- No build-time codegen (this is glue, not data)
- No new shims required
- The `<dom/bindings/hubbub/parser.h>` path issue is the only build-system
  surprise, and the fix is a single wrapper header

## 9. Sequencing, what has to land before what

### Phase 1: Minimal fetcher wiring (no real handlers)

The smallest possible change that lets `netsurf_init` succeed AND a
real HTTP fetcher actually does work:

1. Write `macos9_http_fetcher.c` (the real fetcher_operation_table impl)
2. Write a tiny `macos9_fetcher_init.c` that calls `fetcher_add()` for "http"
3. Delete `fetch_stub.c` and `misc_stub.c::fetcher_init` (let the real
   `content/fetch.c` symbols take over, plus our new init)
4. Add the 2 new files to MacSurf.mcp
5. Verify `hlcache_handle_retrieve("http://...")` returns success but
   the content handler chain still fails because no html_init has
   registered

**Outcome of Phase 1:** the fetcher works end-to-end through NetSurf
core, but no content handler is registered yet, so the page doesn't
render, `hlcache_handle_retrieve` will fail with "no content handler
for type text/html". The fetcher is no longer the bottleneck. Useful
because it isolates the fetch path from the rendering path for
debugging.

### Phase 2: Content handler infrastructure

Add the required NetSurf core helpers BEFORE the handlers themselves:

6. Add `content/content_factory.c` to the build
7. Add the 9-10 utils/ files (`corestrings.c`, `libdom.c`, `talloc.c`,
   etc.), work through compile errors one at a time
8. Add the 4 desktop/ files (`selection.c`, `scrollbar.c`,
   `textarea.c`, `system_colour.c`)
9. Delete the corresponding stubs from `misc_stub.c`
10. Verify the build still compiles and links, we now have the
    plumbing for content handlers but no handlers registered

### Phase 3: CSS handler (small, low-risk)

Do CSS first because it's smaller (5 .c files vs 23) and html depends
on it transitively:

11. Add `content/handlers/css/{css,hints,internal,select,dump}.c` to
    the build
12. Convert designated initializers to assignment-statement form
    (~3 instances expected)
13. Delete `frontends/macos9/css/{css.h,utils.h}` stubs
14. Replace `misc_stub.c::nscss_init` with the real link to the new
    handler
15. Verify `nscss_init()` runs at netsurf_init() time and returns OK

### Phase 4: HTML handler (the big one)

16. Add 23 `content/handlers/html/*.c` files in tier order
17. Convert designated initializers (the `html_content_handler` static
    const + 8 other instances)
18. Convert the 1 for-scope declaration in `layout_flex.c`
19. Add the `<dom/bindings/hubbub/parser.h>` wrapper header
20. Delete `frontends/macos9/html/{html.h,form_internal.h}` stubs
21. Replace `misc_stub.c::html_init` with the real link
22. Verify `html_init()` runs and registers `html_content_handler`

### Phase 5: End-to-end render

23. Wire `browser_window_create` from main.c into the menu's "New
    Window" / URL bar action (already half-done in v0.1)
24. The first navigation should now: call macos9_http_fetcher → bytes
    → llcache → hlcache → html_content_handler → libhubbub parse →
    libdom tree → libcss cascade → content_msg_REDRAW → plotters
25. Implement at least the minimum plotters in
    `frontends/macos9/plotters.c`: `plot_text`, `plot_clip`,
    `plot_rectangle` (currently stubs returning OK without drawing)

## 10. Estimated scope

| Phase | New files in MacSurf.mcp | New code (approx) | Risk |
|---|---:|---:|---|
| 1. Fetcher wiring | 2 | 400 lines | Low (mostly mechanical from prior OT work) |
| 2. Content infrastructure | ~14 | 0 (just adding to build) | Medium (cascading compile errors) |
| 3. CSS handler | 5 | ~50 lines (designated init fixes) | Low |
| 4. HTML handler | 23 | ~150 lines (designated init + for-scope + html_content_handler conversion) | Medium-high (32K LOC, biggest C99 fix yet) |
| 5. Plotters + end-to-end | 0 (modify existing) | ~200 lines (3 QuickDraw plotters) | Medium (first time plotters actually draw) |
| **Total** | **~44** | **~800 lines new + conversions** | |

Combined with the 443 library files already in the project, MacSurf.mcp
will have **~487 .c files** after this milestone, about double the
current count.

## 11. Minimum viable v0.2, one path through the system

If the goal is "any HTML page renders end to end via the real NetSurf
pipeline," the minimum work is:

1. Ship the fetcher (Phase 1), 2 files, ~400 lines
2. Ship the content infrastructure (Phase 2), ~14 files added
3. Ship the CSS handler (Phase 3), 5 files added
4. Ship the HTML handler (Phase 4), 23 files added + conversions
5. Implement `plot_text`, `plot_clip`, `plot_rectangle` in
   plotters.c, 3 functions, ~150 lines of QuickDraw

**That's it.** 44 new project files, ~800 lines of frontend glue, and
the stub deletions. Image rendering deferred to v0.3. JavaScript
deferred to v0.3. Tables/floats/flexbox layout will all WORK because
the libdom + libcss + libhubbub layers handle them, the layout engine
is in NetSurf core's `content/handlers/html/layout.c` (one of the 23
files we're adding).

The most likely v0.2 first-build failure modes:

1. **Designated initializer conversion in `html_content_handler`** ,
   it's 16+ function pointer fields and easy to get the order wrong.
2. **Missing utils/ helper**, a function from `utils/messages.c`,
   `utils/talloc.c`, or `utils/libdom.c` that html.c needs but isn't
   in our build. Linker errors will tell us which.
3. **`<dom/bindings/hubbub/parser.h>` wrapper**, if the wrapper
   header isn't in the right place or doesn't forward correctly, we'll
   see "file not found" errors.
4. **Plotters not actually called**, if the plotter chain is connected
   wrong, we'll see no errors but a blank window. Hardest debug class.

## 12. What this audit does NOT cover

- The actual plotter implementation. Three QuickDraw plotters
  (`plot_text`, `plot_clip`, `plot_rectangle`) are documented as "the
  v0.2 minimum" but the actual code (font metrics, clip-region
  intersection, color conversion to QuickDraw RGB) is its own task.
- The bookmarks/history work from the architecture doc's v0.2 scope.
  That's frontend-only and orthogonal to the core wiring.
- The JS bridge (Standard mode), explicitly deferred to v0.3.
- Image format libraries (libnsgif, libnsbmp, libpng, libjpeg) ,
  deferred to v0.3 by leaving WITH_BMP/etc. undefined.
- Whether NetSurf core's `desktop/browser_window.c` (already in the
  build) has any latent dependencies on the html/css handlers that
  haven't manifested because the stubs were silent. Likely yes ,
  expect compile errors when html_init goes from stub to real and
  the linker realises browser_window.c has been silently using the
  stub function signatures.
- The `talloc` library. `utils/talloc.c` is a Samba-derived hierarchical
  memory allocator. It's in NetSurf's tree but uses POSIX-y patterns.
  May need its own port pass, worth a quick look before Phase 2.

## 13. Open questions

Decisions before implementation:

1. **Phase 1 vs all-at-once.** The 5-phase plan above is incremental.
   Alternative is "do everything in one branch and see what breaks."
   Phase-by-phase is slower but each phase can be committed
   independently. Recommend phased.

2. **Content factory or hand-roll handler registration.** Phase 2 says
   add `content_factory.c`. Alternative: stub `content_factory_register_handler`
   to store handlers in a tiny static array. Saves the cascading deps
   from content_factory's own `#include`s. Trade-off: less faithful to
   how upstream works.

3. **Which talloc.** NetSurf's `utils/talloc.c` is a vendored copy of
   Samba's talloc. We could use it as-is (and audit its POSIX
   dependencies), or replace it with a thin malloc-based wrapper that
   ignores the hierarchy. Trade-off: HTML's box-tree allocation lifetimes
   may rely on the hierarchy for cleanup; replacing talloc could leak.

4. **Backing store: yes or no.** `content/fs_backing_store.c` is in
   the build today. The alternative is `content/no_backing_store.c`
   which is also in the tree. Backing-store-on-disk uses POSIX file
   I/O extensively and may not work cleanly on OS 9 HFS+. Recommend
   switching to `no_backing_store.c` for v0.2.

5. **`<math.h>` in `layout_flex.c`.** Flexbox layout uses `floor()`,
   `ceil()`, possibly `sqrt()`. CW8 MSL has math.h but the link target
   may need `MathLib` added to the project. First-build risk.

## Bottom line

The library chain is done; what's left for v0.2 is **glue, not new
infrastructure**. The work breaks down into:

- **~44 new files in MacSurf.mcp** (2 frontend + 14 content/utils/desktop
  helpers + 5 CSS handler + 23 HTML handler)
- **~800 lines of new/modified frontend code** (the fetcher rewrite +
  the QuickDraw plotters + the misc_stub.c trimming)
- **~25 designated initializer conversions** scattered across html/css
  handler files (same playbook as libdom + libcss)
- **226 lines of stub deletion** (the 4 frontends/macos9/{html,css}/*.h
  files)
- **One path-resolution fix** for `<dom/bindings/hubbub/parser.h>`

The hardest parts will be the **html_content_handler designated init
conversion** (16+ function pointer fields, must preserve order
exactly), the **cascading compile errors** in Phase 2 as we discover
which utils/desktop helpers are needed transitively, and the
**first-render plotter debug** (no errors, just a blank screen if
the plot chain is wrong).

Most likely bottleneck: the talloc question. If talloc-as-vendored
doesn't compile under CW8, that's its own port pass before HTML can
land.

## Files

- HTML handler source: [`browser/netsurf/content/handlers/html/`](../../browser/netsurf/content/handlers/html/), 23 .c, 21 .h, ~32K LOC
- CSS handler source: [`browser/netsurf/content/handlers/css/`](../../browser/netsurf/content/handlers/css/), 5 .c, 6 .h, ~6.6K LOC
- Image handler source: [`browser/netsurf/content/handlers/image/`](../../browser/netsurf/content/handlers/image/), DEFERRED, gated by `#ifdef WITH_*`
- Existing fetcher reference: [`browser/netsurf/content/fetchers/data.c`](../../browser/netsurf/content/fetchers/data.c)
- Existing OT primitives: [`browser/netsurf/frontends/macos9/macos9_fetch.c`](../../browser/netsurf/frontends/macos9/macos9_fetch.c)
- Stubs to delete:
  - `browser/netsurf/frontends/macos9/html/html.h`
  - `browser/netsurf/frontends/macos9/html/form_internal.h`
  - `browser/netsurf/frontends/macos9/css/css.h`
  - `browser/netsurf/frontends/macos9/css/utils.h`
  - `browser/netsurf/frontends/macos9/fetch_stub.c`
  - The relevant `misc_stub.c` function bodies (file stays, individual
    stubs are removed)
- New file to create: `browser/netsurf/frontends/macos9/macos9_http_fetcher.c`
- Wrapper header to create: `browser/libdom/include/dom/bindings/hubbub/parser.h`
- Companion audits: parserutils-port.md, libhubbub-port.md, libdom-port.md,
  libcss-port.md, netsurf-core-integration.md (the original 2026-04
  research that mapped the high-level NetSurf core API).
