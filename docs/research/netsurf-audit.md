# NetSurf Source Audit

Audit of NetSurf and its dependency libraries for the MacSurf project.
Repos cloned via `git://git.netsurf-browser.org/` into `browser/`.

---

## 1. C Standard Targets

All six repositories uniformly target **C99**.

| Repository | C Standard | Evidence |
|---|---|---|
| netsurf | C99 | `test/Makefile` line 142: `-std=c99`; all frontend Makefiles |
| libhubbub | C99 | `Makefile` line 35: `-std=c99`; README: "A C99 capable C compiler" |
| libcss | C99 | `Makefile` line 35: `-std=c99`; README: "A C99 capable C compiler" |
| libdom | C99 | `Makefile` line 38: `-std=c99`; README: "A C99 capable C compiler" |
| libparserutils | C99 | `Makefile` line 33: `-std=c99`; README: "A C99 capable C compiler" |
| libwapcaplet | C99 | `Makefile` line 34: `-std=c99` |

CodeWarrior 8 supports most of C99. The uniform standard is good news for portability.

---

## 2. POSIX Dependencies

### Key Finding: No Threading

**No `pthread.h` includes or `pthread_*` calls exist anywhere in the codebase.** No `fork()`, `exec*()`, or `pipe()` either. This is ideal for Mac OS 9's cooperative multitasking model.

### 2.1 File I/O

| Header/Function | Where Used | Notes |
|---|---|---|
| `unistd.h` | `utils/file.c`, `utils/filepath.c`, `content/fs_backing_store.c`, `content/fetchers/file/file.c` | Core file operations |
| `sys/types.h` | Same files as above + libdom tests, libhubbub perf | Type definitions |
| `sys/stat.h` | Same files as above | File status |
| `fcntl.h` | `content/fs_backing_store.c`, `content/fetchers/file/file.c`, libdom tests | File control flags (O_RDWR, O_CREAT, etc.) |
| `sys/mman.h` | `content/fetchers/file/file.c`, libhubbub perf tests | mmap/munmap for file loading |
| `open()` / `close()` / `read()` / `write()` | `content/fs_backing_store.c`, `content/fetchers/file/file.c` | POSIX file descriptors |
| `mmap()` / `munmap()` | `content/fetchers/file/file.c`, libhubbub perf | Memory-mapped file I/O |
| `fstatat()` | `utils/file.c`, `frontends/riscos/filename.c` | File status relative to dir |
| `opendir()` / `readdir()` / `closedir()` | `utils/utils.c`, `utils/file.c`, riscos/atari/gtk frontends | Directory traversal |
| `access()` | Multiple files | File permission checks |
| `realpath()` | `utils/filepath.c`, windows/atari frontends | Canonical paths |
| `rmdir()` | `utils/file.c` | Directory removal |
| `stat()` / `fstat()` | Throughout | File status |

### 2.2 Networking

| Header/Function | Where Used | Notes |
|---|---|---|
| `sys/socket.h` | `utils/inet.h` | Socket API |
| `netinet/in.h` | `utils/inet.h`, libparserutils test | Internet addresses |
| `arpa/inet.h` | `utils/inet.h`, libparserutils test | IP address conversion |
| `sys/select.h` | `utils/inet.h` | I/O multiplexing |
| `socket()` | `desktop/gui_factory.c` line 549 | Socket creation |
| `select()` | `frontends/monkey/main.c` line 325 | I/O multiplexing |

Note: Networking in production frontends is typically delegated to cURL or platform-specific APIs. For MacSurf, all fetches go through the proxy via plain HTTP, so most networking code can be replaced with Open Transport async calls.

### 2.3 Signals

| Header/Function | Where Used | Notes |
|---|---|---|
| `signal.h` | `desktop/netsurf.c`, monkey/riscos/atari frontends | Signal handling |
| `signal(SIGPIPE, ...)` | `desktop/netsurf.c` line 132 | Ignore broken pipe |
| `signal(SIGSEGV, ...)` etc. | monkey/riscos frontends | Crash handlers |

### 2.4 Time

| Header/Function | Where Used | Notes |
|---|---|---|
| `sys/time.h` | `utils/sys_time.h`, duktape, libdom tests | Time structures |
| `gettimeofday()` | windows/monkey/framebuffer/riscos frontends, `utils/log.c` | Wall-clock time |
| `time()` | `content/fs_backing_store.c`, `content/urldb.c`, `content/llcache.c`, libdom | Current time |
| `localtime()` / `gmtime()` | `desktop/global_history.c`, `utils/time.c`, duktape | Time conversion |
| `mktime()` / `strftime()` / `strptime()` | `utils/time.c`, duktape | Time formatting/parsing |
| `clock_gettime(CLOCK_MONOTONIC)` | duktape | High-res timer |

### 2.5 Character Encoding

| Header/Function | Where Used | Notes |
|---|---|---|
| `iconv_open()` / `iconv()` / `iconv_close()` | `utils/utf8.c`, `libparserutils/src/input/filter.c` | Charset conversion, **critical dependency** |

### 2.6 Dynamic Loading

| Header/Function | Where Used | Notes |
|---|---|---|
| `dlfcn.h` / `dlsym()` | `test/malloc_fig.c` only | Test-only, not needed for production |

### 2.7 Summary by Repository

| Repository | POSIX Usage Level | Primary Dependencies |
|---|---|---|
| **netsurf** | Heavy | File I/O, networking, signals, time, iconv, directory ops |
| **libhubbub** | Light | sys/types.h, sys/stat.h, sys/mman.h (perf tests only) |
| **libcss** | Minimal | sys/types.h (tests only) |
| **libdom** | Light | sys/types.h, unistd.h, fcntl.h, sys/time.h (tests only) |
| **libparserutils** | Minimal | arpa/inet.h, netinet/in.h (tests); iconv (production) |
| **libwapcaplet** | Minimal | signal.h, sys/types.h only |

The five libraries are nearly platform-agnostic, POSIX usage is almost entirely in test code. The main `netsurf` codebase carries all the real POSIX weight, primarily in the file fetcher, backing store, and utility functions.

---

## 3. Platform-Specific #ifdefs

### 3.1 Central Configuration Hub: `utils/config.h`

This is the primary portability layer. All platform conditionals for feature availability are concentrated here:

| Macro | Platforms | What It Controls |
|---|---|---|
| `__riscos__` | RISC OS | Disables STRPTIME, STRFTIME; enables STRCHRNUL; disables MMAP, DIRFD, UNLINKAT, FSTATAT |
| `__MINT__` | Atari MiNT | Same as RISC OS + ceilf workaround |
| `_WIN32` | Windows | Disables SYS_SELECT, POSIX_INET_HEADERS, INETATON, INETPTON, UTSNAME, REALPATH, MKDIR, SIGPIPE, STDOUT; disables MMAP, DIRFD, UNLINKAT, FSTATAT |
| `__amigaos4__` / `__AMIGA__` | AmigaOS 4 / OS 3 | Disables STRTOULL (OS3), MMAP, DIRFD, UNLINKAT, FSTATAT; disables IPv6 |
| `__HAIKU__` / `__BEOS__` | Haiku / BeOS | Custom inttypes; MMAP conditional; disables IPv6 (BeOS only); disables DIRFD, UNLINKAT, FSTATAT |
| `__serenity__` | SerenityOS | Disables INETATON, SCANDIR, REGEX; disables IPv6 |
| `__NetBSD__` | NetBSD | Version checks for features |
| `__OpenBSD__` | OpenBSD | Feature conditionals |
| `__linux__` / `__GLIBC__` | Linux/glibc | HAVE_EXECINFO (backtrace support) |
| `__APPLE__` | macOS | Excluded from certain GNU extensions |

### 3.2 Frontend-Specific Ifdefs

**AmigaOS frontend** (`frontends/amiga/`): Extensive `#ifdef __amigaos4__` throughout, nearly every file distinguishes OS4 vs OS3 APIs for:
- Library opening/management
- Memory allocation
- Font engines (Bullet vs DiskFont)
- Graphics rendering
- Menu construction

**RISC OS frontend** (`frontends/riscos/`): Uses `#ifndef __ELF__` for UnixLib compatibility. Content handlers guarded by `WITH_DRAW`, `WITH_SPRITE`, `WITH_ARTWORKS`.

**Duktape** (`content/handlers/javascript/duktape/duk_config.h`): Extensive platform detection for every major OS including Amiga, RISC OS, Windows, Linux, macOS, BSD variants, MiNT.

### 3.3 Feature Flags (WITH_*)

Used throughout netsurf core for optional features:
- `WITH_PDF_EXPORT`, `WITH_OPENSSL`, `WITH_CURL`
- `WITH_JPEG`, `WITH_PNG`, `WITH_NS_SVG`
- `WITH_AMIGA_DATATYPES`, `WITH_DRAW_EXPORT`
- `WITH_GRESOURCE`, `WITH_BUILTIN_PIXBUF`

### 3.4 Library Platform Ifdefs

| Repository | Ifdefs Found |
|---|---|
| libhubbub | None |
| libcss | None |
| libdom | `__linux__` in `test/normalize.c` (ASAN check) |
| libparserutils | `__riscos` in `test/inputstream.c` |
| libwapcaplet | None |

The libraries are effectively platform-neutral. All portability work will be in the netsurf core.

---

## 4. NetSurf Frontend API

All headers from `include/netsurf/`. Functions are divided into **callback tables** (frontend must implement) and **callable functions** (frontend calls into the core).

### 4.1 Callback Tables (Frontend Must Implement)

#### gui_bitmap_table (bitmap.h)

```c
void *(*create)(int width, int height, enum gui_bitmap_flags flags);
void (*destroy)(void *bitmap);
void (*set_opaque)(void *bitmap, bool opaque);
bool (*get_opaque)(void *bitmap);
unsigned char *(*get_buffer)(void *bitmap);
size_t (*get_rowstride)(void *bitmap);
int (*get_width)(void *bitmap);
int (*get_height)(void *bitmap);
void (*modified)(void *bitmap);
nserror (*render)(struct bitmap *bitmap, struct hlcache_handle *content);
```

#### gui_clipboard_table (clipboard.h)

```c
void (*get)(char **buffer, size_t *length);
void (*set)(const char *buffer, size_t length, nsclipboard_styles styles[], int n_styles);
```

#### core_window_table (core_window.h)

```c
nserror (*invalidate)(struct core_window *cw, const struct rect *rect);
nserror (*set_extent)(struct core_window *cw, int width, int height);
nserror (*set_scroll)(struct core_window *cw, int x, int y);
nserror (*get_scroll)(const struct core_window *cw, int *x, int *y);
nserror (*get_dimensions)(const struct core_window *cw, int *width, int *height);
nserror (*drag_status)(struct core_window *cw, core_window_drag_status ds);
```

#### gui_download_table (download.h)

```c
struct gui_download_window *(*create)(struct download_context *ctx, struct gui_window *parent);
nserror (*data)(struct gui_download_window *dw, const char *data, unsigned int size);
void (*error)(struct gui_download_window *dw, const char *error_msg);
void (*done)(struct gui_download_window *dw);
```

#### gui_fetch_table (fetch.h)

```c
/* Mandatory */
const char *(*filetype)(const char *unix_path);

/* Optional */
struct nsurl *(*get_resource_url)(const char *path);
nserror (*get_resource_data)(const char *path, const uint8_t **data, size_t *data_len);
nserror (*release_resource_data)(const uint8_t *data);
char *(*mimetype)(const char *ro_path);
int (*socket_open)(int domain, int type, int protocol);
int (*socket_close)(int socket);
```

#### gui_layout_table (layout.h)

```c
nserror (*width)(const struct plot_font_style *fstyle, const char *string, size_t length, int *width);
nserror (*position)(const struct plot_font_style *fstyle, const char *string, size_t length, int x, size_t *char_offset, int *actual_x);
nserror (*split)(const struct plot_font_style *fstyle, const char *string, size_t length, int x, size_t *char_offset, int *actual_x);
```

#### gui_misc_table (misc.h)

```c
/* Mandatory */
nserror (*schedule)(int t, void (*callback)(void *p), void *p);

/* Optional */
void (*quit)(void);
nserror (*launch_url)(struct nsurl *url);
nserror (*login)(struct nsurl *url, const char *realm, const char *username, const char *password,
                 nserror (*cb)(struct nsurl *, const char *, const char *, const char *, void *), void *cbpw);
void (*pdf_password)(char **owner_pass, char **user_pass, char *path);
nserror (*present_cookies)(const char *search_term);
```

#### plotter_table (plotters.h)

```c
nserror (*clip)(const struct redraw_context *ctx, const struct rect *clip);
nserror (*arc)(const struct redraw_context *ctx, const plot_style_t *pstyle, int x, int y, int radius, int angle1, int angle2);
nserror (*disc)(const struct redraw_context *ctx, const plot_style_t *pstyle, int x, int y, int radius);
nserror (*line)(const struct redraw_context *ctx, const plot_style_t *pstyle, const struct rect *line);
nserror (*rectangle)(const struct redraw_context *ctx, const plot_style_t *pstyle, const struct rect *rectangle);
nserror (*polygon)(const struct redraw_context *ctx, const plot_style_t *pstyle, const int *p, unsigned int n);
nserror (*path)(const struct redraw_context *ctx, const plot_style_t *pstyle, const float *p, unsigned int n, const float transform[6]);
nserror (*bitmap)(const struct redraw_context *ctx, struct bitmap *bitmap, int x, int y, int width, int height, colour bg, bitmap_flags_t flags);
nserror (*text)(const struct redraw_context *ctx, const plot_font_style_t *fstyle, int x, int y, const char *text, size_t length);
/* Optional */
nserror (*group_start)(const struct redraw_context *ctx, const char *name);
nserror (*group_end)(const struct redraw_context *ctx);
nserror (*flush)(const struct redraw_context *ctx);
bool option_knockout;
```

#### gui_search_table (search.h)

```c
void (*status)(bool found, void *p);
void (*hourglass)(bool active, void *p);
void (*add_recent)(const char *string, void *p);
void (*forward_state)(bool active, void *p);
void (*back_state)(bool active, void *p);
```

#### gui_utf8_table (utf8.h)

```c
nserror (*utf8_to_local)(const char *string, size_t len, char **result);
nserror (*local_to_utf8)(const char *string, size_t len, char **result);
```

#### gui_window_table (window.h)

```c
/* Mandatory */
struct gui_window *(*create)(struct browser_window *bw, struct gui_window *existing, gui_window_create_flags flags);
void (*destroy)(struct gui_window *gw);
nserror (*invalidate)(struct gui_window *gw, const struct rect *rect);
bool (*get_scroll)(struct gui_window *gw, int *sx, int *sy);
nserror (*set_scroll)(struct gui_window *gw, const struct rect *rect);
nserror (*get_dimensions)(struct gui_window *gw, int *width, int *height);
nserror (*event)(struct gui_window *gw, enum gui_window_event event);

/* Optional */
void (*set_title)(struct gui_window *gw, const char *title);
nserror (*set_url)(struct gui_window *gw, struct nsurl *url);
void (*set_icon)(struct gui_window *gw, struct hlcache_handle *icon);
void (*set_status)(struct gui_window *g, const char *text);
void (*set_pointer)(struct gui_window *g, enum gui_pointer_shape shape);
void (*place_caret)(struct gui_window *g, int x, int y, int height, const struct rect *clip);
bool (*drag_start)(struct gui_window *g, gui_drag_type type, const struct rect *rect);
nserror (*save_link)(struct gui_window *g, struct nsurl *url, const char *title);
void (*create_form_select_menu)(struct gui_window *gw, struct form_control *control);
void (*file_gadget_open)(struct gui_window *gw, struct hlcache_handle *hl, struct form_control *gadget);
void (*drag_save_object)(struct gui_window *gw, struct hlcache_handle *c, gui_save_type type);
void (*drag_save_selection)(struct gui_window *gw, const char *selection);
void (*console_log)(struct gui_window *gw, browser_window_console_source src, const char *msg, size_t msglen, browser_window_console_flags flags);
```

### 4.2 Callable Functions (Frontend Calls Into Core)

#### browser.h

```c
nserror browser_set_dpi(int dpi);
int browser_get_dpi(void);
```

#### browser_window.h

```c
/* Creation and navigation */
nserror browser_window_create(enum browser_window_create_flags flags, struct nsurl *url, struct nsurl *referrer, struct browser_window *existing, struct browser_window **bw);
nserror browser_window_navigate(struct browser_window *bw, struct nsurl *url, struct nsurl *referrer, enum browser_window_nav_flags flags, char *post_urlenc, struct fetch_multipart_data *post_multipart, struct hlcache_handle *parent);
bool browser_window_up_available(struct browser_window *bw);
nserror browser_window_navigate_up(struct browser_window *bw, bool new_window);

/* Accessors */
struct nsurl *browser_window_access_url(const struct browser_window *bw);
nserror browser_window_get_url(struct browser_window *bw, bool fragment, struct nsurl **url_out);
const char *browser_window_get_title(struct browser_window *bw);
struct history *browser_window_get_history(struct browser_window *bw);
nserror browser_window_get_extents(struct browser_window *bw, bool scaled, int *width, int *height);
bool browser_window_has_content(struct browser_window *bw);
struct hlcache_handle *browser_window_get_content(struct browser_window *bw);
float browser_window_get_scale(struct browser_window *bw);
nserror browser_window_get_features(struct browser_window *bw, int x, int y, struct browser_window_features *data);
browser_drag_type browser_window_get_drag_type(struct browser_window *bw);
browser_editor_flags browser_window_get_editor_flags(struct browser_window *bw);
bool browser_window_can_select(struct browser_window *bw);
char *browser_window_get_selection(struct browser_window *bw);
bool browser_window_can_search(struct browser_window *bw);
bool browser_window_is_frameset(struct browser_window *bw);
nserror browser_window_get_scrollbar_type(struct browser_window *bw, browser_scrolling *h, browser_scrolling *v);
nserror browser_window_get_name(struct browser_window *bw, const char **name);
browser_window_page_info_state browser_window_get_page_info_state(const struct browser_window *bw);
nserror browser_window_get_ssl_chain(struct browser_window *bw, struct cert_chain **chain);
int browser_window_get_cookie_count(const struct browser_window *bw);

/* Actions */
void browser_window_set_dimensions(struct browser_window *bw, int width, int height);
void browser_window_stop(struct browser_window *bw);
nserror browser_window_reload(struct browser_window *bw, bool all);
void browser_window_destroy(struct browser_window *bw);
void browser_window_reformat(struct browser_window *bw, bool background, int width, int height);
nserror browser_window_set_scale(struct browser_window *bw, float scale, bool absolute);
nserror browser_window_refresh_url_bar(struct browser_window *bw);
nserror browser_window_schedule_reformat(struct browser_window *bw);
void browser_window_set_pointer(struct browser_window *bw, browser_pointer_shape shape);
void browser_window_page_drag_start(struct browser_window *bw, int x, int y);
void browser_window_set_drag_type(struct browser_window *bw, browser_drag_type type, const struct rect *rect);
nserror browser_window_set_name(struct browser_window *bw, const char *name);
void browser_window_set_position(struct browser_window *bw, int x, int y);

/* Input */
void browser_window_mouse_click(struct browser_window *bw, browser_mouse_state mouse, int x, int y);
void browser_window_mouse_track(struct browser_window *bw, browser_mouse_state mouse, int x, int y);
bool browser_window_key_press(struct browser_window *bw, uint32_t key);
bool browser_window_scroll_at_point(struct browser_window *bw, int x, int y, int scrx, int scry);
bool browser_window_drop_file_at_point(struct browser_window *bw, int x, int y, char *file);
void browser_window_set_gadget_filename(struct browser_window *bw, struct form_control *gadget, const char *fn);

/* Navigation state */
bool browser_window_back_available(struct browser_window *bw);
bool browser_window_forward_available(struct browser_window *bw);
bool browser_window_reload_available(struct browser_window *bw);
bool browser_window_stop_available(struct browser_window *bw);
struct browser_window *browser_window_find_target(struct browser_window *bw, const char *target, browser_mouse_state mouse);

/* Rendering */
bool browser_window_redraw(struct browser_window *bw, int x, int y, const struct rect *clip, const struct redraw_context *ctx);
bool browser_window_redraw_ready(struct browser_window *bw);
void browser_window_get_position(struct browser_window *bw, bool root, int *pos_x, int *pos_y);

/* Debug and misc */
nserror browser_window_debug_dump(struct browser_window *bw, FILE *f, enum content_debug op);
nserror browser_window_debug(struct browser_window *bw, enum content_debug op);
bool browser_window_exec(struct browser_window *bw, const char *src, size_t srclen);
nserror browser_window_console_log(struct browser_window *bw, browser_window_console_source src, const char *msg, size_t msglen, browser_window_console_flags flags);
nserror browser_window_show_cookies(const struct browser_window *bw);
nserror browser_window_show_certificates(struct browser_window *bw);
```

#### content.h

```c
struct bitmap *content_get_bitmap(struct hlcache_handle *h);
const char *content_get_encoding(struct hlcache_handle *h, enum content_encoding_type op);
lwc_string *content_get_mime_type(struct hlcache_handle *h);
const uint8_t *content_get_source_data(struct hlcache_handle *h, size_t *size);
const char *content_get_title(struct hlcache_handle *h);
content_type content_get_type(struct hlcache_handle *h);
int content_get_width(struct hlcache_handle *h);
int content_get_height(struct hlcache_handle *h);
void content_invalidate_reuse_data(struct hlcache_handle *h);
bool content_redraw(struct hlcache_handle *h, struct content_redraw_data *data, const struct rect *clip, const struct redraw_context *ctx);
bool content_scaled_redraw(struct hlcache_handle *h, int width, int height, const struct redraw_context *ctx);
struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle);
```

#### cookie_db.h

```c
void urldb_iterate_cookies(bool (*callback)(const struct cookie_data *cookie));
void urldb_delete_cookie(const char *domain, const char *path, const char *name);
nserror urldb_load_cookies(const char *filename);
nserror urldb_save_cookies(const char *filename);
```

#### form.h

```c
nserror form_select_process_selection(struct form_control *control, int item);
struct form_option *form_select_get_option(struct form_control *control, int item);
char *form_control_get_name(struct form_control *control);
nserror form_control_bounding_rect(struct form_control *control, struct rect *r);
```

#### netsurf.h

```c
nserror netsurf_register(struct netsurf_table *table);
nserror netsurf_init(const char *store_path);
void netsurf_exit(void);
```

#### ssl_certs.h

```c
nserror cert_chain_alloc(size_t depth, struct cert_chain **chain_out);
nserror cert_chain_dup_into(const struct cert_chain *src, struct cert_chain *dst);
nserror cert_chain_dup(const struct cert_chain *src, struct cert_chain **dst_out);
nserror cert_chain_from_query(struct nsurl *url, struct cert_chain **chain_out);
nserror cert_chain_to_query(struct cert_chain *chain, struct nsurl **url_out);
nserror cert_chain_free(struct cert_chain *chain);
size_t cert_chain_size(const struct cert_chain *chain);
```

#### url_db.h

```c
nserror urldb_load(const char *filename);
nserror urldb_save(const char *filename);
void urldb_iterate_partial(const char *prefix, bool (*callback)(struct nsurl *url, const struct url_data *data));
void urldb_iterate_entries(bool (*callback)(struct nsurl *url, const struct url_data *data));
const struct url_data *urldb_get_url_data(struct nsurl *url);
void urldb_set_cert_permissions(struct nsurl *url, bool permit);
void urldb_dump(void);
```

### 4.3 Type-Only Headers (No Functions)

- **console.h**, `browser_window_console_source`, `browser_window_console_flags` enums
- **content_type.h**, `content_type`, `content_status`, `content_msg` enums
- **css.h**, Color conversion macros
- **inttypes.h**, Integer format macros (with RISC OS special handling)
- **mouse.h**, `browser_mouse_state`, `gui_pointer_shape`, `browser_pointer_shape` enums
- **plot_style.h**, Plot styles, font styles, color macros
- **types.h**, `colour`, `struct rect`

---

## 5. RISC OS Frontend Structure

`frontends/riscos/`, the closest analog to Mac OS 9. Cooperative multitasking via the WIMP event loop. All files listed below.

### Core

| File | Description |
|---|---|
| gui.c | Main GUI initialization, WIMP event loop, startup/shutdown |
| gui.h | Main GUI interface and constants |
| window.c | Browser window handling, create, destroy, redraw, scroll, events |
| window.h | Browser window interface |
| corewindow.c | Generic core window rendering adapter for WIMP |
| corewindow.h | Core window interface |
| toolbar.c | Window toolbar implementation |
| toolbar.h | Toolbar interface |
| schedule.c | Scheduled callback queue (timed deferred work) |
| assert.c | Assert failure reporting and exit |

### Rendering

| File | Description |
|---|---|
| plotters.c | Screen plotter, draws lines, rectangles, text, bitmaps via OS calls |
| bitmap.c | Bitmap operations using RISC OS sprites |
| bitmap.h | Bitmap operations interface |
| buffer.c | Screen buffering (double-buffer) implementation |
| buffer.h | Screen buffering interface |
| font.c | Font handling using RUfl (RISC OS Unicode font library) |
| font.h | Font handling interface |
| image.c | Image display and handling |
| image.h | Image handling interface |
| palettes.c | Sprite color palette definitions |
| palettes.h | Sprite palette interface |

### UI Widgets (gui/ subdirectory)

| File | Description |
|---|---|
| gui/button_bar.c | Button bar / toolbar buttons |
| gui/button_bar.h | Button bar interface |
| gui/progress_bar.c | Loading progress bar |
| gui/progress_bar.h | Progress bar interface |
| gui/status_bar.c | UTF-8 status bar |
| gui/status_bar.h | Status bar interface |
| gui/throbber.c | Animated loading indicator |
| gui/throbber.h | Throbber interface |
| gui/url_bar.c | URL bar with address input |
| gui/url_bar.h | URL bar interface |

### Navigation Features

| File | Description |
|---|---|
| cookies.c | Cookie manager window |
| cookies.h | Cookie manager interface |
| global_history.c | Global history window |
| global_history.h | Global history interface |
| local_history.c | Per-window local history |
| local_history.h | Local history interface |
| hotlist.c | Hotlist/bookmarks manager |
| hotlist.h | Hotlist interface |
| url_complete.c | URL auto-completion |
| url_complete.h | URL completion interface |
| url_suggest.c | URL suggestion menu |
| url_suggest.h | URL suggestion interface |
| search.c | In-page text search |
| search.h | Search interface |
| searchweb.c | Web search provider integration |
| pageinfo.c | Page information core window |
| pageinfo.h | Page info interface |

### Dialogs and Menus

| File | Description |
|---|---|
| dialog.c | Dialog box creation and management |
| dialog.h | Dialog box interface |
| menus.c | Menu creation and event handling |
| menus.h | Menu definitions and interface |
| iconbar.c | Iconbar icon and menu handling |
| iconbar.h | Iconbar interface |
| query.c | Query dialog for user confirmation |
| query.h | Query dialog interface |

### Configuration (configure/ subdirectory)

| File | Description |
|---|---|
| configure.c | Option setting implementation |
| configure.h | Option setting interface |
| configure/configure.h | Configuration options interface |
| configure/con_cache.c | Cache configuration |
| configure/con_connect.c | Connection configuration |
| configure/con_content.c | Content type configuration |
| configure/con_fonts.c | Font configuration |
| configure/con_home.c | Home page configuration |
| configure/con_image.c | Image display configuration |
| configure/con_inter.c | Interface configuration |
| configure/con_language.c | Language preference configuration |
| configure/con_search.c | Search provider configuration |
| configure/con_secure.c | Security configuration |
| configure/con_theme.c | Theme configuration |

### Content Handlers (content-handlers/ subdirectory)

| File | Description |
|---|---|
| content-handlers/artworks.c | Handler for image/x-artworks format |
| content-handlers/artworks.h | Artworks handler interface |
| content-handlers/draw.c | Handler for image/x-drawfile format |
| content-handlers/draw.h | DrawFile handler interface |
| content-handlers/sprite.c | Handler for RISC OS sprite format |
| content-handlers/sprite.h | Sprite handler interface |
| content-handlers/awrender.s | ARM assembly for Artworks rendering |

### File and Text Operations

| File | Description |
|---|---|
| download.c | Download window implementation |
| filetype.c | File type identification from content |
| filetype.h | File type definitions |
| filename.c | Unique filename generation |
| filename.h | Filename generation interface |
| save.c | Save dialog and drag-drop saving |
| save.h | Save dialog interface |
| save_draw.c | Export content as DrawFile |
| save_draw.h | DrawFile export interface |
| save_pdf.c | Export content as PDF |
| save_pdf.h | PDF export interface |
| print.c | Printing support |
| print.h | Printing interface |
| textarea.c | Single/multi-line UTF-8 text input widget |
| textarea.h | Text area interface |
| textselection.c | Text selection import/export |
| textselection.h | Text selection interface |
| ucstables.c | UCS conversion tables and UTF-8 handling |
| ucstables.h | UCS conversion interface |
| clipboard, | (handled via textselection) |

### System Integration

| File | Description |
|---|---|
| help.c | Interactive help message handling |
| help.h | Help interface |
| message.c | Automated RISC OS message routing |
| message.h | Message routing interface |
| mouse.c | Mouse dragging and tracking support |
| mouse.h | Mouse support interface |
| wimp.c | General WIMP/OS library functions |
| wimp.h | WIMP functions interface |
| wimp_event.c | Automated WIMP event handling |
| wimp_event.h | WIMP event interface |
| wimputils.h | WIMP API utility functions |
| uri.c | RISC OS URI message protocol |
| uri.h | URI protocol interface |
| url_protocol.c | ANT URL launching protocol |
| url_protocol.h | ANT URL protocol interface |
| theme.c | Window themes and styling |
| theme.h | Theme styles and interface |
| theme_install.c | Theme auto-installation |
| tinct.h | Tinct SWI numbers and color handling |
| oslib_pre7.h | OSLib 7 backward compatibility defines |
| options.h | RISC OS-specific option definitions |

### Build and Resources

| File | Description |
|---|---|
| Makefile | Build configuration |
| Makefile.defaults | Default build settings |
| Makefile.tools | Build tools configuration |
| appdir/ | Application resource files (sprites, templates, help, fonts) |
| distribution/ | Distribution and library files |
| scripts/ | Boot and initialization scripts |
| templates/ | WIMP template files (en, de, fr, nl) |

---

## 6. AmigaOS Frontend Structure

`frontends/amiga/`, also cooperative multitasking. Distinguishes OS3 and OS4 throughout with `#ifdef __amigaos4__`.

### Core

| File | Description |
|---|---|
| gui.c / gui.h | Main GUI initialization, window management, event handling |
| gui_menu.c / gui_menu.h | Amiga menu system construction and management |
| gui_options.c / gui_options.h | Options/preferences dialog GUI |
| corewindow.c / corewindow.h | Core window adapter for Amiga Intuition drawable surfaces |
| schedule.c / schedule.h | Event scheduler with timed callback management |
| version.c / version.h | Version information and build number |

### Rendering

| File | Description |
|---|---|
| plotters.c / plotters.h | Graphics rendering backend for web page display |
| bitmap.c / bitmap.h | Bitmap image handling and format conversion |
| font.c / font.h | Core font rendering and DPI calculation |
| font_bullet.c / font_bullet.h | Bullet font engine (OS3) initialization |
| font_diskfont.c / font_diskfont.h | DiskFont rendering system (OS4+) |
| font_cache.c / font_cache.h | Font cache with time-based expiration |
| font_scan.c / font_scan.h | Font glyph scanner for Unicode substitution |
| rtg.c / rtg.h | RTG (Picasso96) graphics abstraction layer |

### Navigation Features

| File | Description |
|---|---|
| cookies.c / cookies.h | Cookie viewer window |
| history.c / history.h | Global history viewer |
| history_local.c / history_local.h | Per-window local history |
| hotlist.c / hotlist.h | Hotlist/bookmarks viewer |
| search.c / search.h | In-page text search |
| pageinfo.c / pageinfo.h | Page information viewer |
| selectmenu.c / selectmenu.h | HTML form select dropdown handler |
| ctxmenu.c / ctxmenu.h | Context menu (right-click) implementation |

### File and Download Operations

| File | Description |
|---|---|
| download.c / download.h | Download manager with progress tracking |
| file.c / file.h | File dialog and save operations via ASL requesters |
| filetype.c / filetype.h | MIME type to file type mapping |
| save_pdf.c / save_pdf.h | PDF export via Haru library |
| print.c / print.h | Printer driver interface |

### Content Handlers

| File | Description |
|---|---|
| icon.c / icon.h | Amiga .info icon image format handler |
| iff_dr2d.c / iff_dr2d.h | DR2D IFF vector format handler (SVG conversion) |
| iff_cset.h | IFF CSET chunk structure for character set |
| datatypes.c / datatypes.h | DataTypes format handler registration |
| dt_anim.c | DataTypes animation format handler |
| dt_picture.c | DataTypes picture format handler |
| dt_sound.c | DataTypes sound format handler |
| plugin_hack.c / plugin_hack.h | Plugin handler for external programs |

### Text and Encoding

| File | Description |
|---|---|
| clipboard.c / clipboard.h | Clipboard copy/paste operations |
| utf8.c / utf8.h | UTF-8 to local Amiga charset conversion |
| nsoption.c / nsoption.h | User option file reading and writing |
| drag.c / drag.h | Drag and drop operations |

### System Integration

| File | Description |
|---|---|
| arexx.c / arexx.h | ARexx scripting support for automation |
| help.c / help.h | Help system with AmigaGuide integration |
| launch.c / launch.h | File URI launch and external program execution |
| libs.c / libs.h | Shared library management and version checking |
| memory.c / memory.h | OS3 memory allocation and slab allocator |
| misc.c / misc.h | Utility functions and error reporting |
| object.c / object.h | Generic object list management |
| os3support.c / os3support.h | OS3 compatibility function implementations |
| theme.c / theme.h | Theme/skin system for toolbar graphics and cursors |

### UI Components

| File | Description |
|---|---|
| agclass/amigaguide_class.c / .h | BOOPSI class for AmigaGuide help display |
| stringview/stringview.c / .h | Custom StringView gadget class for URL input |
| stringview/urlhistory.c / .h | URL history list for string view |
| menu.c / menu.h | Classic GadTools menu construction |
| hash/xxhash.c / .h | xxHash fast hash algorithm |
| options.h | Amiga-specific option definitions |

### Build, Resources, and Distribution

| File | Description |
|---|---|
| Makefile | Build configuration |
| Makefile.defaults | Default compiler options |
| Makefile.tools | Build tool paths |
| resources/ | CSS defaults, MIME types, splash, favicons, pointers, themes |
| dist/ | Install scripts, AmigaGuide help, ARexx automation scripts |
| pkg/ | Package creation scripts, READMEs, search engine config |

---

## Key Takeaways for MacSurf

1. **C99 everywhere**, good fit for CodeWarrior 8 which supports most of C99
2. **No threading**, the codebase already assumes cooperative multitasking. No pthread adaptation needed
3. **Libraries are clean**, libhubbub/libcss/libdom/libparserutils/libwapcaplet have almost zero platform dependencies outside of test code
4. **iconv is critical**, used in both netsurf core and libparserutils for charset conversion. Will need a Mac OS 9 replacement (TextEncoding Converter)
5. **File I/O is the biggest POSIX surface**, mmap, opendir/readdir, fstatat all need Carbon/Toolbox replacements
6. **RISC OS frontend is the template**, 65+ files, fully cooperative, WIMP event loop maps directly to WaitNextEvent
7. **AmigaOS frontend validates the approach**, OS3/OS4 split proves the pattern works for old cooperative OSes
8. **`utils/config.h` is the portability switch**, add `__MACOS9__` guards there alongside existing platform macros
