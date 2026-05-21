# MacSurf Frontend Scaffold Status

Scaffold created at `browser/netsurf/frontends/macos9/`. All files compile cleanly against NetSurf core headers (verified with `gcc -fsyntax-only -std=c99`). No Mac Toolbox calls, pure stubs returning sensible defaults.

---

## Files Created

### macos9.h, Shared declarations

Defines `struct gui_window`, `struct gui_download_window`, and scheduler function prototypes shared across all frontend files.

### main.c, Entry point and event loop

- `main()`, builds `struct netsurf_table`, calls `netsurf_register()`, `netsurf_init()`, runs event loop, calls `netsurf_exit()`
- `macos9_poll()`, empty event loop stub (will become WaitNextEvent loop)

### window.c, gui_window_table (13 callbacks)

| Function | Signature | Return |
|---|---|---|
| `create` | `(struct browser_window *bw, struct gui_window *existing, gui_window_create_flags flags)` | `struct gui_window *` (allocated) |
| `destroy` | `(struct gui_window *gw)` | void |
| `invalidate` | `(struct gui_window *gw, const struct rect *rect)` | `NSERROR_OK` |
| `get_scroll` | `(struct gui_window *gw, int *sx, int *sy)` | `true` |
| `set_scroll` | `(struct gui_window *gw, const struct rect *rect)` | `NSERROR_OK` |
| `get_dimensions` | `(struct gui_window *gw, int *width, int *height)` | `NSERROR_OK` |
| `event` | `(struct gui_window *gw, enum gui_window_event event)` | `NSERROR_OK` |
| `set_title` | `(struct gui_window *gw, const char *title)` | void |
| `set_url` | `(struct gui_window *gw, struct nsurl *url)` | `NSERROR_OK` |
| `set_icon` | `(struct gui_window *gw, struct hlcache_handle *icon)` | void |
| `set_status` | `(struct gui_window *g, const char *text)` | void |
| `set_pointer` | `(struct gui_window *g, enum gui_pointer_shape shape)` | void |
| `place_caret` | `(struct gui_window *g, int x, int y, int height, const struct rect *clip)` | void |
| `drag_start` | `(struct gui_window *g, gui_drag_type type, const struct rect *rect)` | `false` |
| `save_link` | `(struct gui_window *g, struct nsurl *url, const char *title)` | `NSERROR_OK` |
| `create_form_select_menu` | `(struct gui_window *gw, struct form_control *control)` | void |
| `file_gadget_open` | `(struct gui_window *gw, struct hlcache_handle *hl, struct form_control *gadget)` | void |
| `drag_save_object` | `(struct gui_window *gw, struct hlcache_handle *c, gui_save_type type)` | void |
| `drag_save_selection` | `(struct gui_window *gw, const char *selection)` | void |
| `console_log` | `(struct gui_window *gw, browser_window_console_source src, const char *msg, size_t msglen, browser_window_console_flags flags)` | void |

### macos9_bitmap.c, gui_bitmap_table (10 callbacks)

| Function | Return |
|---|---|
| `create` | Allocated `struct macos9_bitmap *` with RGBA buffer |
| `destroy` | void (frees buffer + struct) |
| `set_opaque` | void |
| `get_opaque` | `bool` |
| `get_buffer` | `unsigned char *` (RGBA data pointer) |
| `get_rowstride` | `size_t` (width * 4) |
| `get_width` | `int` |
| `get_height` | `int` |
| `modified` | void (no-op) |
| `render` | `NSERROR_OK` |

### plotters.c, plotter_table (9 mandatory + 3 optional)

| Function | Return |
|---|---|
| `clip` | `NSERROR_OK` |
| `arc` | `NSERROR_OK` |
| `disc` | `NSERROR_OK` |
| `line` | `NSERROR_OK` |
| `rectangle` | `NSERROR_OK` |
| `polygon` | `NSERROR_OK` |
| `path` | `NSERROR_OK` |
| `bitmap` | `NSERROR_OK` |
| `text` | `NSERROR_OK` |
| `group_start` | NULL (optional, not implemented) |
| `group_end` | NULL (optional, not implemented) |
| `flush` | NULL (optional, not implemented) |
| `option_knockout` | `true` |

### font.c, gui_layout_table (3 callbacks)

| Function | Return |
|---|---|
| `width` | `NSERROR_OK` (stub: 8px per char) |
| `position` | `NSERROR_OK` (stub: 8px per char) |
| `split` | `NSERROR_OK` (stub: 8px per char, walks back to space) |

### macos9_fetch.c, gui_fetch_table (3 callbacks)

| Function | Return |
|---|---|
| `filetype` | MIME string from extension lookup table |
| `get_resource_url` | `NULL` |
| `mimetype` | `strdup()` of filetype result |

### clipboard.c, gui_clipboard_table (2 callbacks)

| Function | Return |
|---|---|
| `get` | void (sets buffer=NULL, length=0) |
| `set` | void (no-op) |

### macos9_utf8.c, gui_utf8_table (2 callbacks)

| Function | Return |
|---|---|
| `utf8_to_local` | `NSERROR_OK` (stub: copies input unchanged) |
| `local_to_utf8` | `NSERROR_OK` (stub: copies input unchanged) |

### macos9_download.c, gui_download_table (4 callbacks)

| Function | Return |
|---|---|
| `create` | Allocated `struct gui_download_window *` |
| `data` | `NSERROR_OK` |
| `error` | void |
| `done` | void (frees struct) |

### misc.c, gui_misc_table (5 callbacks)

| Function | Return |
|---|---|
| `schedule` | Delegates to `macos9_schedule()` in schedule.c |
| `quit` | void (sets `macos9_done = true`) |
| `launch_url` | `NSERROR_OK` |
| `login` | `NSERROR_NOT_IMPLEMENTED` |
| `pdf_password` | void (no-op) |
| `present_cookies` | `NSERROR_NOT_IMPLEMENTED` |

### schedule.c, Scheduler stub

- `macos9_schedule()`, linked list insert/remove with ms-to-ticks conversion
- `macos9_schedule_run()`, dispatches all due callbacks from queue head
- `macos9_get_ticks()`, stub returning 0 (will become `TickCount()`)

### Makefile.macos9, Build configuration

- Sets `-std=c99 -D__MACOS9__ -DWITHOUT_DUKTAPE -DNO_IPV6`
- Lists all 11 source files in `S_FRONTEND`
- Modeled on `frontends/riscos/Makefile`

---

## Compilation Results

All 11 `.c` files pass `gcc -fsyntax-only -std=c99` against the NetSurf include tree with zero errors and zero warnings.

```
=== main.c ===       ✓ clean
=== window.c ===     ✓ clean
=== macos9_bitmap.c ===     ✓ clean
=== plotters.c ===   ✓ clean
=== font.c ===       ✓ clean
=== macos9_fetch.c ===      ✓ clean
=== clipboard.c ===  ✓ clean
=== macos9_utf8.c ===       ✓ clean
=== macos9_download.c ===   ✓ clean
=== misc.c ===       ✓ clean
=== schedule.c ===   ✓ clean
```

---

## Tables Not Yet Implemented (Optional, NULL in netsurf_table)

| Table | Header | Status |
|---|---|---|
| `core_window_table` | `core_window.h` | NULL, deferred (ancillary windows: cookies, history, bookmarks) |
| `gui_file_table` | `file.h` | NULL, uses default POSIX impl (will need shims) |
| `gui_search_table` | `search.h` | NULL, deferred (page text search UI) |
| `gui_search_web_table` | `search_web.h` | NULL, deferred (web search provider) |
| `gui_llcache_table` | `llcache.h` | NULL, uses default impl |

---

## What This Scaffold Does Not Include

- No Mac Toolbox calls (Carbon, QuickDraw, Open Transport), pure C stubs
- No POSIX shim layer, those are separate files per posix-portability.md
- No `utils/config.h` `__MACOS9__` block yet, needed before full compilation
- No resource files, icons, or application bundle structure
- No actual rendering, font measurement, or networking
