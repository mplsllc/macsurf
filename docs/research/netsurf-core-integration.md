# NetSurf Core Integration for MacSurf

## Purpose

Map the path from where MacSurf is today (HTTP fetched by a standalone
`macos9_fetch_url` in [macos9_fetch.c](../../browser/netsurf/frontends/macos9/macos9_fetch.c)
that bypasses NetSurf's core, tag-stripped with a regex-level stripper, displayed as
plain text) to the path we actually want: `netsurf_init` → `browser_window_create` →
`hlcache_handle_retrieve` → content messages → plotters → rendered HTML in the window.

**This document is research, not a plan.** No code decisions are made here; it catalogs
API signatures, sequencing, and stub state so the design work can happen next.

> Note: `docs/milestone-v0.1.0.md` does not exist in the repo, the v0.1.0 milestone
> report was written in a chat session but never committed as a file. This document
> treats the current source tree and commit `b6e872a` as ground truth.

---

## 1. Where we are today

[browser/netsurf/frontends/macos9/macos9_fetch.c](../../browser/netsurf/frontends/macos9/macos9_fetch.c)
does all of its own networking:

```c
void macos9_fetch_url(const char *url, struct gui_window *gw,
        void (*callback)(struct gui_window *gw,
            const char *data, long len, int status));
```

It opens an Open Transport endpoint, calls `OTSnd`/`OTRcv` inline, strips HTTP headers,
strips HTML tags with `macos9_strip_html()`, word-wraps, and writes the plain text into
`gw->content` for [window.c](../../browser/netsurf/frontends/macos9/window.c)'s update
handler to draw. None of this traverses NetSurf's core. There is no `browser_window`,
no `hlcache_handle`, no fetcher, no plotter pipeline. The current `struct gui_fetch_table
macos9_fetch_table` registers only type sniffers (`filetype`, `mimetype`); the active
fields are all the frontend-directory ones.

This got us v0.1.0. To get layout, CSS, images, and a real browser, we need to stop
bypassing core and start driving it.

---

## 2. `netsurf_init`

**Header:** [include/netsurf/netsurf.h:46](../../browser/netsurf/include/netsurf/netsurf.h#L46)
**Implementation:** [desktop/netsurf.c:110](../../browser/netsurf/desktop/netsurf.c#L110)

```c
nserror netsurf_init(const char *store_path);
```

`netsurf_init` itself calls, in order:

1. `corestrings_init()`, string pool
2. `nscolour_update()`, color table
3. `image_cache_init()`, image cache parameters
4. `nscss_init()`, CSS handler
5. `html_init()`, HTML handler
6. `image_init()`, image content handler
7. `textplain_init()`, plain-text content handler
8. `setlocale(LC_ALL, "")`
9. `fetcher_init()`, initialize registered fetchers
10. `hlcache_initialise()`, high-level cache (calls llcache_initialise)
11. `ns_system_colour_init()`
12. `js_initialise()`, JS engine (stubbed in MacSurf)
13. `page_info_init()`

### Prerequisites that must run BEFORE `netsurf_init`

Verified against [frontends/riscos/gui.c:2460-2515](../../browser/netsurf/frontends/riscos/gui.c#L2460)
and [frontends/amiga/gui.c:6568-6662](../../browser/netsurf/frontends/amiga/gui.c#L6568):

1. **`netsurf_register(&macos9_table)`**, populates the global `guit` pointer.
   See [desktop/gui_factory.c:777](../../browser/netsurf/desktop/gui_factory.c#L777).
   MacSurf already does this at main.c:460.
2. **`nsoption_init(set_defaults, &nsoptions, &nsoptions_default)`**, options system.
   MacSurf does **not** call this today. There is a `// TODO: nsoption_init` comment at
   main.c around the fetch init.
3. **`messages_add_from_file(path)`**, localized message table. Used by the init
   functions (`html_init`, `nscss_init`, etc.) to format status strings. MacSurf
   does not call this.
4. (Optional but typical) **`nsoption_read("NetSurf:Choices", NULL)`**, load options file.
5. (Optional) **`nsoption_commandline()`**, parse CLI args. MacSurf does not need this.

### `struct netsurf_table`, required sub-tables

From [desktop/gui_table.h:48-164](../../browser/netsurf/desktop/gui_table.h#L48) and the
validation logic in [gui_factory.c:794-868](../../browser/netsurf/desktop/gui_factory.c#L794):

| Field | Required | MacSurf status | Notes |
|---|---|---|---|
| `misc` | **yes** | `&macos9_misc_table` | Core/browser misc hooks |
| `window` | **yes** | `macos9_window_table` | Frontend window ops |
| `fetch` | **yes** | `&macos9_fetch_table` | Type sniffers, NOT HTTP fetcher |
| `bitmap` | **yes** | `macos9_bitmap_table` | Image format callbacks |
| `layout` | **yes** | `macos9_layout_table` | Font/text measurement |
| `corewindow` | no | not set | Defaults to `default_corewindow_table` |
| `download` | no | `macos9_download_table` | Defaults to `default_download_table` |
| `clipboard` | no | `macos9_clipboard_table` | Defaults to `default_clipboard_table` |
| `file` | no | not set | Defaults to `default_file_table` |
| `utf8` | no | `macos9_utf8_table` | Defaults to `default_utf8_table` |
| `search` | no | not set | May be NULL |
| `search_web` | no | not set | May be NULL |
| `llcache` | no | not set | Low-level cache backend |

All five mandatory tables are already populated in
[main.c:449-461](../../browser/netsurf/frontends/macos9/main.c#L449). netsurf_register
will succeed today.

### Does `netsurf_init` survive MacSurf's stubs?

**Yes, but dangerously.** Every initializer it calls is stubbed to return
`NSERROR_OK` without doing anything real:

- [frontends/macos9/misc_stub.c](../../browser/netsurf/frontends/macos9/misc_stub.c)
  stubs `image_cache_init`, `html_init`, `image_init`, `nscss_init`,
  `ns_system_colour_init`, `fetcher_init`, `page_info_init` to all return `NSERROR_OK`
  without side effects.
- [frontends/macos9/fetch_stub.c](../../browser/netsurf/frontends/macos9/fetch_stub.c)
  stubs `fetch_start` → NULL, `fetch_can_fetch` → false, `fetcher_quit` → no-op.
- [frontends/macos9/js_stub.c](../../browser/netsurf/frontends/macos9/js_stub.c)
  stubs `js_newheap` → `*heap = NULL`.

Result: `netsurf_init(NULL)` returns `NSERROR_OK` on MacSurf today, but the system has no
HTML parser, no CSS parser, no image decoder, no registered fetchers, and no content
handlers. The first call to `hlcache_handle_retrieve` will fail with
`NSERROR_NO_FETCH_HANDLER` because `fetch_can_fetch("http://")` returns false.

`corestrings_init` is **actually implemented** in
[corestrings_stub.c](../../browser/netsurf/frontends/macos9/corestrings_stub.c) and
[lwc_stub.c](../../browser/netsurf/frontends/macos9/lwc_stub.c), despite the names,
these files implement `lwc_intern_string` and friends. So core strings work.

---

## 3. `browser_window_create`

**Header:** [include/netsurf/browser_window.h:195](../../browser/netsurf/include/netsurf/browser_window.h#L195)
**Implementation:** [desktop/browser_window.c:3081](../../browser/netsurf/desktop/browser_window.c#L3081)

```c
nserror browser_window_create(enum browser_window_create_flags flags,
                              struct nsurl *url,
                              struct nsurl *referrer,
                              struct browser_window *existing,
                              struct browser_window **bw);
```

### Flags

From [include/netsurf/browser_window.h:98-127](../../browser/netsurf/include/netsurf/browser_window.h#L98):

```c
enum browser_window_create_flags {
    BW_CREATE_NONE             = 0,
    BW_CREATE_HISTORY          = (1 << 0),
    BW_CREATE_TAB              = (1 << 1),
    BW_CREATE_CLONE            = (1 << 2),
    BW_CREATE_UNVERIFIABLE     = (1 << 3),
    BW_CREATE_FOREGROUND       = (1 << 4),
    BW_CREATE_FOCUS_LOCATION   = (1 << 5)
};
```

RISC OS passes `BW_CREATE_HISTORY` for a fresh top-level window
([riscos/gui.c:748](../../browser/netsurf/frontends/riscos/gui.c#L748)). Amiga does the
same for new windows and `BW_CREATE_CLONE | BW_CREATE_HISTORY` for duplicates
([amiga/gui.c:1472, 1525](../../browser/netsurf/frontends/amiga/gui.c#L1472)).

### How the frontend's `gui_window` is wired up

`browser_window_create` calls through the registered table at
[browser_window.c:3142-3149](../../browser/netsurf/desktop/browser_window.c#L3142):

```c
ret->window = guit->window->create(ret,
    (existing != NULL) ? existing->window : NULL,
    gw_flags);
```

So the core **asks the frontend to create a `gui_window`** via
`macos9_window_table->create(bw, existing_gw, flags)`. The frontend returns its existing
or newly-allocated `struct gui_window`, and the core holds onto the pointer for the
lifetime of the `browser_window`. No callback pointer is passed at create time; all
further events come through the content-message callback registered internally by
`browser_window_navigate`.

If the `url` argument is non-NULL, `browser_window_create` immediately calls
`browser_window_navigate` internally at
[browser_window.c:3160](../../browser/netsurf/desktop/browser_window.c#L3160).

---

## 4. The fetcher system, the actual integration point

**Header:** [content/fetchers.h:49-113](../../browser/netsurf/content/fetchers.h#L49)
**Fetch message types:** [content/fetch.h:42-56](../../browser/netsurf/content/fetch.h#L42)

### `struct fetcher_operation_table`

```c
struct fetcher_operation_table {
    bool (*initialise)(lwc_string *scheme);
    bool (*acceptable)(const struct nsurl *url);
    void *(*setup)(struct fetch *parent_fetch, struct nsurl *url,
                   bool only_2xx, bool downgrade_tls,
                   const char *post_urlenc,
                   const struct fetch_multipart_data *post_multipart,
                   const char **headers);
    bool (*start)(void *fetch);
    void (*abort)(void *fetch);
    void (*free)(void *fetch);
    void (*poll)(lwc_string *scheme);
    int (*fdset)(lwc_string *scheme, fd_set *read_set,
                 fd_set *write_set, fd_set *error_set);
    void (*finalise)(lwc_string *scheme);
};
```

Registered via [content/fetchers.h:113](../../browser/netsurf/content/fetchers.h#L113):

```c
nserror fetcher_add(lwc_string *scheme, const struct fetcher_operation_table *ops);
```

`fetcher_add` must be called **before** `netsurf_init`, because `netsurf_init → fetcher_init`
iterates registered fetchers to initialize them per-scheme. In RISC OS and Amiga this is
done by the frontend between `netsurf_register` and `netsurf_init`; MacSurf never does it
(the stub returns OK without registering anything).

### Fetch message types

From [content/fetch.h:42-56](../../browser/netsurf/content/fetch.h#L42):

```c
typedef enum {
    FETCH_PROGRESS,
    FETCH_CERTS,
    FETCH_HEADER,
    FETCH_DATA,
    /* Anything after here is a completed fetch */
    FETCH_FINISHED,
    FETCH_TIMEDOUT,
    FETCH_ERROR,
    FETCH_REDIRECT,
    FETCH_NOTMODIFIED,
    FETCH_AUTH,
    FETCH_CERT_ERR,
    FETCH_SSL_ERR
} fetch_msg_type;
```

The custom fetcher's `start()` / `poll()` functions feed these messages back to core via
the parent_fetch pointer received in `setup()`. The fetcher doesn't invent its own
transport contract, it just drives OT and translates the results into these messages.

### Where RISC OS and Amiga put their HTTP fetcher

**Not visible in the MacSurf source subset.** RISC OS and Amiga in the upstream tree use
`content/fetchers/curl.c` (libcurl-based) and `content/fetchers/file.c` (file:// URLs)
and `content/fetchers/about.c` (about: URLs). These are compile-time choices in the
upstream makefiles. For MacSurf, neither libcurl nor POSIX file I/O is available, we
need a Mac-specific HTTP fetcher that wraps Open Transport.

**Key design implication:** The existing `macos9_fetch.c` is not far from what a fetcher
backend needs. Its guts, `OTOpenEndpointInContext`, `OTConnect`, `OTSnd`, `OTRcv`,
`OTSndOrderlyDisconnect`, map onto the `setup`/`start`/`poll` callbacks. The body-parser
layer (HTTP header split, tag strip, word wrap) goes away because the core does all of
that once we're feeding bytes in via `FETCH_DATA` messages.

---

## 5. `hlcache_handle_retrieve`, bytes become content

**Header:** [content/hlcache.h:119](../../browser/netsurf/content/hlcache.h#L119)

```c
nserror hlcache_handle_retrieve(nsurl *url, uint32_t flags,
                                nsurl *referer,
                                llcache_post_data *post,
                                hlcache_handle_callback cb,
                                void *pw,
                                hlcache_child_context *child,
                                content_type accepted_types,
                                hlcache_handle **result);
```

Callback signature ([hlcache.h:63](../../browser/netsurf/content/hlcache.h#L63)):

```c
typedef nserror (*hlcache_handle_callback)(hlcache_handle *handle,
                                           const hlcache_event *event,
                                           void *pw);
```

```c
typedef struct hlcache_event {
    content_msg type;
    union content_msg_data data;
} hlcache_event;
```

### Content messages

From [include/netsurf/content_type.h:103-178](../../browser/netsurf/include/netsurf/content_type.h#L103):
`CONTENT_MSG_LOG`, `CONTENT_MSG_SSL_CERTS`, `CONTENT_MSG_LOADING`, `CONTENT_MSG_READY`,
`CONTENT_MSG_DONE`, `CONTENT_MSG_ERROR`, `CONTENT_MSG_REDIRECT`, `CONTENT_MSG_STATUS`,
`CONTENT_MSG_REFORMAT`, `CONTENT_MSG_REDRAW`, `CONTENT_MSG_REFRESH`, `CONTENT_MSG_DOWNLOAD`,
`CONTENT_MSG_LINK`, `CONTENT_MSG_GETTHREAD`, `CONTENT_MSG_GETDIMS`, `CONTENT_MSG_SCROLL`,
`CONTENT_MSG_DRAGSAVE`, `CONTENT_MSG_SAVELINK`, `CONTENT_MSG_POINTER`,
`CONTENT_MSG_SELECTION`, `CONTENT_MSG_CARET`, `CONTENT_MSG_DRAG`, `CONTENT_MSG_SELECTMENU`,
`CONTENT_MSG_GADGETCLICK`, `CONTENT_MSG_TEXTSEARCH`.

### Does the frontend handle `hlcache_handle` directly?

**Not in the common path.** The frontend calls `browser_window_create` /
`browser_window_navigate`, and those internally allocate their own `hlcache_handle` and
register `browser_window_callback`
([browser_window.c:1450](../../browser/netsurf/desktop/browser_window.c#L1450)) as the
content-msg sink. `browser_window_callback` dispatches on message type:

- `CONTENT_MSG_READY` → format/reflow, compute layout
- `CONTENT_MSG_DONE` → finalize, update title, set status
- `CONTENT_MSG_REFORMAT` → recalculate layout, queue redraw
- `CONTENT_MSG_REDRAW` → call the frontend's `invalidate` through the gui_window table

The frontend's `gui_window_table` methods get called for things like `set_title`,
`set_status`, `update_extent`, `invalidate`. The frontend's `plotter_table` callbacks
get called from inside `browser_window_redraw` when the window eventually needs to draw.

Direct `hlcache_handle_retrieve` is only needed for non-browser-window fetches (e.g.
favicons, CSS assets inside an HTML page, iframe contents). For a top-level page load,
`browser_window_navigate` is the entry point.

---

## 6. Plotters, currently all stubs

**Header:** [include/netsurf/plotters.h:102](../../browser/netsurf/include/netsurf/plotters.h#L102)
**MacSurf impl:** [frontends/macos9/plotters.c](../../browser/netsurf/frontends/macos9/plotters.c)

```c
struct plotter_table {
    nserror (*clip)(const struct redraw_context *ctx, const struct rect *clip);
    nserror (*arc)(ctx, pstyle, x, y, radius, angle1, angle2);
    nserror (*disc)(ctx, pstyle, x, y, radius);
    nserror (*line)(ctx, pstyle, line_rect);
    nserror (*rectangle)(ctx, pstyle, rect);
    nserror (*polygon)(ctx, pstyle, points, num_points);
    nserror (*path)(ctx, pstyle, path_data, num_elements, transform[6]);
    nserror (*bitmap)(ctx, bitmap, x, y, width, height, bg, flags);
    nserror (*text)(ctx, font_style, x, y, text_utf8, text_len);
    nserror (*group_start)(ctx);
    nserror (*group_end)(ctx);
    nserror (*flush)(ctx);
    bool option_knockout;
};
```

Every plotter in [plotters.c](../../browser/netsurf/frontends/macos9/plotters.c) returns
`NSERROR_OK` without drawing. They have `TODO` comments pointing at the QuickDraw calls
that should replace them:

- `macos9_plot_clip` → `ClipRect()`
- `macos9_plot_arc` → `FrameArc()`
- `macos9_plot_disc` → `PaintOval()` / `FrameOval()`
- `macos9_plot_line` → `MoveTo()` + `LineTo()`
- `macos9_plot_rectangle` → `PaintRect()` / `FrameRect()`
- `macos9_plot_polygon` → `OpenPoly()` / `LineTo()` / `ClosePoly()` / `PaintPoly()`
- `macos9_plot_path` → bezier flattening (hard)
- `macos9_plot_bitmap` → `CopyBits()` + compositing
- `macos9_plot_text` → `TextFont()` + `TextSize()` + `DrawText()`

**Minimum viable set for HTML text:** `clip`, `rectangle` (for backgrounds and borders),
`text`. Images add `bitmap`. Full CSS adds `line`, `disc`, `arc`, `polygon`, `path`.

---

## 7. Required stubs to replace

For core integration to produce visible output, these stubs must become real:

| File | Symbol | Current | Needed |
|---|---|---|---|
| `misc_stub.c` | `fetcher_init` | returns OK, no-op | Call `fetcher_add("http", ...)` or delegate to the real `fetcher_init()` from `content/fetch.c` |
| `misc_stub.c` | `html_init` | returns OK, no-op | Real html content handler. Requires libdom + libhubbub + libwapcaplet + libcss to actually parse HTML |
| `misc_stub.c` | `nscss_init` | returns OK, no-op | Requires libcss working |
| `misc_stub.c` | `image_init` | returns OK, no-op | Requires libnsgif / libnsbmp / libpng / libjpeg. We can likely skip images for v0.2 and add later |
| `fetch_stub.c` | `fetch_can_fetch` | returns false | Must return true for "http://" scheme after real fetcher registered |
| `fetch_stub.c` | `fetch_start` | returns NULL | Delegates to real fetcher's setup/start pair |
| `plotters.c` | `plot_text`, `plot_clip`, `plot_rectangle` | no-op | Must call QuickDraw |

**The elephant in the room:** libcss, libdom, libhubbub, libwapcaplet are currently
stubbed via header stubs in
[frontends/macos9/libcss/](../../browser/netsurf/frontends/macos9/),
[frontends/macos9/dom/](../../browser/netsurf/frontends/macos9/),
[frontends/macos9/libwapcaplet/](../../browser/netsurf/frontends/macos9/), and
[frontends/macos9/parserutils/](../../browser/netsurf/frontends/macos9/). Those are
**headers-only stubs**; the libraries themselves are not in the project. For HTML
parsing and CSS layout to work, those libraries must be built as part of the CW8
project, a large amount of work, potentially another milestone on its own.

Without those libraries, `html_init` can only be a no-op, and the HTML handler cannot
register. Without a registered HTML handler, `hlcache_handle_retrieve(http://..., HTML)`
fails with no content handler.

---

## 8. What RISC OS does, step by step

[riscos/gui.c main(), lines 2440-2543](../../browser/netsurf/frontends/riscos/gui.c#L2440):

```
 1. netsurf_register(&riscos_table)
 2. nslog_init(...)
 3. nsoption_init(set_defaults, &nsoptions, &nsoptions_default)
 4. nsoption_read("NetSurf:Choices", NULL)
 5. nsoption_commandline(&argc, argv, NULL)
 6. ro_gui_choose_language()
 7. messages_add_from_file(path_to_Messages)
 8. cachepath = get_cachepath()
 9. netsurf_init(cachepath)
10. artworks_init() / draw_init() / sprite_init()  (RISC OS content handlers)
11. messages_add_from_file("NetSurf:Resources.LangNames")
12. gui_init(argc, argv)                           (opens initial windows; calls browser_window_create)
13. while (!riscos_done) riscos_poll();            (main loop)
14. netsurf_exit();
```

## 9. What Amiga does, step by step

[amiga/gui.c main(), lines 6540-6700+](../../browser/netsurf/frontends/amiga/gui.c#L6540):

```
 1. netsurf_register(&amiga_table)
 2. ami_libs_open()
 3. ami_gui_splash_open()
 4. ami_gui_resources_open()
 5. ami_gui_read_all_tooltypes()
 6. nsoption_init(ami_set_options, ...)
 7. ami_nsoption_read()
 8. nsoption_commandline(&nargc, nargv, NULL)
 9. ami_locate_resource(messages, "Messages")
10. messages_add_from_file(messages)
11. netsurf_init(current_user_cache)
12. amiga_icon_init(), search_web_init(), ami_clipboard_init(),
    ami_openurl_open(), ami_font_init()
13. urldb_load(...) / urldb_load_cookies(...)
14. gui_init2(argc, argv)                          (calls browser_window_create)
15. main loop
```

Common shape: **register → options → messages → netsurf_init → frontend-specific init →
browser_window_create → event loop**. MacSurf's [main.c](../../browser/netsurf/frontends/macos9/main.c)
currently has `netsurf_register` and `netsurf_init` but is missing steps 3 (nsoption_init)
and 7 (messages_add_from_file). The Messages file itself isn't present in the CW8
project, it needs to be either embedded, stored alongside the app, or stubbed at the
messages subsystem level.

---

## 10. Integration cost, rough categories

What will need to change in MacSurf to get from v0.1.0 to "real core" operational:

### Cheap (< 1 day each)
- Add `nsoption_init` call in main.c
- Replace `fetcher_init` stub with a real `fetcher_add("http", &macos9_http_fetcher_ops)`
- Implement `plot_text`, `plot_clip`, `plot_rectangle` with QuickDraw
- Add a stubbed `messages_add_from_file` that returns NSERROR_OK without loading anything
  (we can defer real message loading by returning "NetSurf" for any message key)
- Wire `browser_window_create` into `macos9_handle_menu` File→New Window flow

### Medium (days to a week each)
- Write `macos9_http_fetcher.c`, fetcher_operation_table backed by Open Transport.
  The network primitives from [macos9_fetch.c](../../browser/netsurf/frontends/macos9/macos9_fetch.c)
  move here; the header split and tag strip code is deleted (core does that now).
- Implement Carbon font metrics properly in `macos9_layout_table`, currently stubbed.
  NetSurf calls `layout->width` / `layout->position` / `layout->split` to measure text
  for layout; these must return real pixel widths from QuickDraw.
- Event loop integration: the fetcher's `poll` needs to run every time through
  `macos9_poll()`, and the fetcher needs to yield via the same `OTUseSyncIdleEvents`
  notifier pattern we use today.

### Large (weeks, separate milestone)
- **libcss, libdom, libhubbub, libwapcaplet, libparserutils** must be real, not stubs.
  These are 5 C libraries that must be added to the CW8 project and made to build under
  C89 with no POSIX. That is a milestone of its own. Without them `html_init` can't
  register a handler, so `hlcache_handle_retrieve` on text/html fails with
  "no content handler for type."
- Image libraries (libnsgif, libnsbmp, libpng, libjpeg), optional for first HTML render,
  required for anything pretty.

### Deferred indefinitely
- libjs / duktape, by design, MacSurf has no JavaScript
- libssl / libcurl, proxy handles TLS

---

## 11. Open questions

Things this document does **not** answer; they need decisions before implementation:

1. **Do we land the HTTP fetcher wiring before or after libcss/libdom?** Options:
   - (a) Ship the HTTP fetcher alone first, even though the content handler chain will
     fail at HTML parse. Gives us a working `fetch_start` and event-loop integration
     without committing to the 5-library dependency port.
   - (b) Land libcss/libdom first as their own milestone, then wire fetcher. Means no
     visible progress until both land.
   - (c) Write a stub HTML handler in MacSurf that ignores CSS/layout and just dumps
     the body text, reusing `macos9_strip_html`. Unblocks incremental progress but
     reuses the v0.1.0 tag stripper temporarily.
2. **`nsoption_init` defaults function:** RISC OS has `set_defaults`, Amiga has
   `ami_set_options`. What does MacSurf's look like? What options do we override?
3. **Messages file:** embed as a Mac resource (`STR#`?), ship as a data file next to
   the app, or write a `messages_get` shim that returns the key unchanged?
4. **Font metrics:** QuickDraw's `StringWidth` and `GetFontInfo` can drive
   `macos9_layout_table`, but font lookup (`FMGetFontFamilyFromName`) needs a UTF-8 →
   Pascal bridge. Where does that bridge live?
5. **hlcache_handle retention:** The frontend may want direct `hlcache_handle` for
   downloads, non-document resources, or favicons. Is v0.2 scope limited to top-level
   HTML only?

---

## 12. References

All paths are in this repo under [browser/netsurf/](../../browser/netsurf/):

- `include/netsurf/netsurf.h`, `netsurf_register`, `netsurf_init`
- `include/netsurf/browser_window.h`, `browser_window_create`, create flags
- `include/netsurf/plotters.h`, `struct plotter_table`
- `include/netsurf/content_type.h`, `content_msg` enum
- `desktop/netsurf.c`, `netsurf_init` implementation
- `desktop/browser_window.c`, `browser_window_create`, `browser_window_callback`
- `desktop/gui_factory.c`, `netsurf_register` validation and defaults
- `desktop/gui_table.h`, `struct netsurf_table`
- `content/hlcache.h`, `hlcache_handle_retrieve`, callback type
- `content/fetchers.h`, `fetcher_operation_table`, `fetcher_add`
- `content/fetch.h`, `fetch_msg_type` enum
- `frontends/riscos/gui.c`, reference `main()` sequence
- `frontends/amiga/gui.c`, reference `main()` sequence
- `frontends/macos9/main.c`, current MacSurf entry point
- `frontends/macos9/macos9_fetch.c`, current OT fetch (bypasses core)
- `frontends/macos9/misc_stub.c`, all-OK initializer stubs
- `frontends/macos9/fetch_stub.c`, fetch subsystem stubs
- `frontends/macos9/plotters.c`, plot_* stubs

## 13. What's next

This document is research only. The next step is a **design discussion**: pick an answer
to question 1 (fetcher-first vs libs-first vs stub-handler) and outline a v0.2 milestone
scope before writing any code. Open questions 2-5 become subtasks once the overall
path is chosen.
