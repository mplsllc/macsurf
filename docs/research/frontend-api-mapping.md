# NetSurf Frontend API Mapping, Mac OS 9

Mapping of every NetSurf frontend callback table function to its Mac OS 9 Carbon/Toolbox equivalent, based on the RISC OS reference implementation. Produced from the [netsurf-audit.md](netsurf-audit.md) findings and direct reading of the RISC OS frontend source.

---

## Section 1, Callback Table Mapping

### gui_window_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `create` | Create a new browser window | `xwimp_create_window()` with WIMP window struct; registers event handlers via `ro_gui_wimp_event_*` | `CreateNewWindow()` + `InstallWindowEventHandler()` | High | Must set up Carbon Event handlers, toolbar, status bar, scroll bars. Single-window initially simplifies this |
| `destroy` | Destroy browser window and free resources | `xwimp_delete_window()`; unlinks from window list | `DisposeWindow()` | Low | Cleanup linked list of gui_window structs |
| `invalidate` | Mark rectangular region for redraw | `xwimp_force_redraw()` on window region | `InvalWindowRect()` | Low | Triggers redraw via Carbon Event loop |
| `get_scroll` | Get current scroll position | `xwimp_get_window_state()` and read `xscroll`/`yscroll` | `GetControlValue()` on h/v scroll bar controls | Low | |
| `set_scroll` | Set scroll position | `xwimp_open_window()` with modified scroll offsets | `SetControlValue()` + `InvalWindowRect()` | Low | |
| `get_dimensions` | Get content area dimensions in pixels | `xwimp_get_window_state()`, compute visible area minus toolbar | `GetWindowBounds(kWindowContentRgn)` minus toolbar height, convert from pixels | Low | Must account for toolbar and scroll bars |
| `event` | Handle core-initiated events (throbber start/stop, etc.) | Switch on event type; updates toolbar throbber, triggers reformat | Switch on event type; update toolbar controls | Medium | Central dispatch for START_THROBBER, STOP_THROBBER, PAGE_INFO_CHANGE, etc. |
| `set_title` | Set window title bar text | `strncpy` to title buffer; WIMP redraws automatically | `SetWTitle()` or `SetWindowTitleWithCFString()` | Low | |
| `set_url` | Update URL bar contents | `ro_toolbar_set_url()`, writes to toolbar URL icon | `SetControlData()` on EditText/EditUnicodeText control in toolbar | Low | |
| `set_icon` | Set favicon in toolbar | `ro_toolbar_set_site_favicon()` | Draw favicon `PixMap` into toolbar area; custom control | Medium | No native favicon control in OS 9, custom drawing |
| `set_status` | Set status bar text | `ro_gui_status_bar_set_text()`, UTF-8 status bar widget | `SetControlData()` on StaticText control or custom status bar | Low | |
| `set_pointer` | Change mouse cursor shape | Lookup in `ro_gui_pointer_table[]`; `xwimpspriteop_set_pointer_shape()` | `SetCursor()` with cursor resource (`'CURS'`/`'crsr'`) | Low | Need to map `gui_pointer_shape` enum to cursor resource IDs |
| `place_caret` | Position text input caret | `xwimp_set_caret_position()` | `SetKeyboardFocus()` + custom caret drawing via `TESetSelect()` or manual | Medium | No direct equivalent to WIMP caret; need custom I-beam drawing in content area |
| `drag_start` | Begin a drag operation | `ro_mouse_drag_start()` + `xwimp_drag_box()` | Not implemented initially | Low | V1 can return false (drag not supported) |
| `save_link` | Save a link to file | RISC OS file save dialog | `NavPutFile()` or skip | Low | Optional; can defer |
| `create_form_select_menu` | Show dropdown for HTML `<select>` | Build `wimp_menu` from form options; `xwimp_create_menu()` | `CreateNewMenu()` + `InsertMenuItem()` + `PopUpMenuSelect()` | Medium | Need to iterate form_control options, build popup menu |
| `file_gadget_open` | Open file chooser for `<input type=file>` | Not implemented in RISC OS | `NavGetFile()` | Low | Optional |
| `drag_save_object` | Drag-save content to desktop | RISC OS drag-save protocol | Not applicable on OS 9 | Low | Stub, no desktop drag-save on OS 9 |
| `drag_save_selection` | Drag-save text selection | RISC OS claim/data protocol | Not applicable | Low | Stub |
| `console_log` | Forward console messages | Updates status or logs | Log to file or ignore | Low | Optional; debugging only |

### gui_bitmap_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `create` | Allocate bitmap of given size | `calloc` for `struct bitmap`; lazy sprite allocation | `NewGWorld()` at 32bpp, or `malloc` RGBA buffer | Low | RISC OS defers sprite init to `get_buffer`; Mac can do same with lazy GWorld |
| `destroy` | Free bitmap and pixel data | `free(sprite_area)` + `free(bitmap)` | `DisposeGWorld()` or `free` buffer | Low | |
| `set_opaque` | Mark bitmap as fully opaque | Sets `bitmap->opaque` flag | Set flag on bitmap struct | Low | Just a flag |
| `get_opaque` | Query opacity state | Returns `bitmap->opaque` | Return flag | Low | |
| `get_buffer` | Get pointer to RGBA pixel data | Returns pointer into sprite area at offset 16+44 (sprite header) | `GetPixBaseAddr(GetGWorldPixMap(gworld))` or return raw RGBA buffer pointer | Low | Pixel format must be 0xAARRGGBB or 0xRRGGBBAA, verify byte order for PPC big-endian |
| `get_rowstride` | Get bytes per pixel row | `width * 4` | `GetPixRowBytes(GetGWorldPixMap(gworld)) & 0x3FFF` or `width * 4` | Low | |
| `get_width` | Get bitmap width | Returns `bitmap->width` | Return stored width | Low | |
| `get_height` | Get bitmap height | Returns `bitmap->height` | Return stored height | Low | |
| `modified` | Notify bitmap data changed | No-op in RISC OS | Invalidate any cached representation | Low | May need to flush GWorld caches |
| `render` | Render content into bitmap (for thumbnails) | Switch output to sprite via `osspriteop_switch_output_to_sprite`; call `content_scaled_redraw` | `SetGWorld()` to offscreen GWorld; call `content_scaled_redraw`; restore GWorld | Medium | Must set up plotter context targeting the GWorld, then restore |

### plotter_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `clip` | Set clipping rectangle | `xos_writen()` with VDU graphics window command | `ClipRect()` or `SetClip()` via `RgnHandle` + `RectRgn()` | Low | |
| `arc` | Draw arc segment | `xcolourtrans_set_gcol()` + `xos_plot(os_PLOT_ARC)` | `FrameArc()` | Low | Map angles correctly, QuickDraw uses 0=12 o'clock clockwise |
| `disc` | Draw filled/outlined circle | `xos_plot(os_PLOT_CIRCLE)` / `os_PLOT_CIRCLE_OUTLINE` | `PaintOval()` / `FrameOval()` with square Rect | Low | |
| `line` | Draw line with style | `xdraw_stroke()` with draw path | `MoveTo()` + `LineTo()` with `PenSize()` | Low | Dotted/dashed via `PenPat()` |
| `rectangle` | Draw filled/outlined rectangle | `xos_plot(os_PLOT_RECTANGLE)` for fill; `xdraw_stroke()` for outline | `PaintRect()` / `FrameRect()` | Low | |
| `polygon` | Draw filled polygon | `xdraw_fill()` with constructed path | `OpenPoly()` + `LineTo()` + `ClosePoly()` + `PaintPoly()` | Low | |
| `path` | Draw bezier path with transforms | `xdraw_fill()` / `xdraw_stroke()` with path + transform matrix | QuickDraw has no native bezier; flatten to line segments, or use `ATSUDrawText` path (OS X only). Manual bezier subdivision required | High | Most complex plotter. Must implement cubic bezier flattening. Rare in practice for simple pages |
| `bitmap` | Plot bitmap image, optionally tiled | `image_redraw()` via Tinct SWI; handles tiling and alpha | `CopyBits()` or `DrawPixMap()` from GWorld; manual tiling loop | Medium | Alpha blending requires manual compositing on OS 9 (no built-in alpha CopyBits). Tiling needs loop |
| `text` | Draw text string | `xcolourtrans_set_font_colours()` + `rufl_paint()` | `RGBForeColor()` + `DrawText()` / `ATSUDrawText()` (ATSUI if available) or `TextFont()` + `TextSize()` + `DrawString()` | Medium | UTF-8 to Mac encoding conversion needed before drawing. See gui_utf8_table |
| `group_start` | Begin named drawing group | Optional; not implemented in RISC OS | Stub, return `NSERROR_OK` | Low | For SVG group boundaries; not needed |
| `group_end` | End named drawing group | Optional; not implemented in RISC OS | Stub | Low | |
| `flush` | Flush drawing operations | Optional; not implemented in RISC OS | `QDFlushPortBuffer()` if available, else no-op | Low | |
| `option_knockout` | Enable knockout rendering optimization | Set to `true` in RISC OS | Set to `true` | Low | Reduces overdraw |

### gui_layout_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `width` | Measure string width in pixels | `rufl_width()` via RUfl font library | `TextWidth()` after `TextFont()` + `TextSize()` + `TextFace()` | Medium | Must map CSS font style to Mac font family/size. Handle UTF-8 → Mac encoding |
| `position` | Find character offset at x coordinate | `rufl_x_to_offset()` | `PixelToChar()` (if using ATSUI) or manual binary search with `TextWidth()` | Medium | Binary search approach: measure substrings until x coordinate found |
| `split` | Find word-break point fitting width | `rufl_split()` + scan for spaces | `TextWidth()` + manual word-break scan | Medium | Same as RISC OS: find pixel split point, then walk back to nearest space |

### gui_misc_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `schedule` | Schedule callback after t milliseconds | `riscos_schedule()`, linked list of callbacks sorted by `os_read_monotonic_time()` | `InstallEventLoopTimer()` or manual linked-list + `TickCount()` / `Microseconds()` | Low | Same pattern as RISC OS: linked list checked each WaitNextEvent iteration. Convert ms to ticks |
| `quit` | Clean shutdown | Saves options, cookies, URL database; frees resources | Same, save prefs to file, call `netsurf_exit()` | Low | |
| `launch_url` | Open URL in external browser | RISC OS URI protocol dispatch | `ICLaunchURL()` via Internet Config, or `LSOpenCFURLRef()` (OS X). On OS 9: Internet Config API | Low | |
| `login` | HTTP authentication dialog | Not implemented in RISC OS | `StandardAlert()` or custom dialog with username/password fields | Medium | Need modal dialog with two text fields |
| `pdf_password` | PDF password prompt | Not implemented in RISC OS | Not needed, no PDF export planned | Low | Stub |
| `present_cookies` | Open cookie manager window | `ro_gui_cookies_present()`, opens RISC OS cookie viewer | Custom window listing cookies from `urldb_iterate_cookies()` | Medium | Defer to v2; stub initially |

### gui_fetch_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `filetype` | Map file path to MIME type | `fetch_filetype()`, maps RISC OS file type numbers to MIME strings | Map file extension to MIME string via lookup table | Low | Simple extension-to-MIME mapping (`.html` → `text/html`, etc.) |
| `get_resource_url` | Convert resource path to file URL | Maps to `file:///NetSurf:/Resources/` paths with language lookup | Map to `file:///` path within application bundle/folder | Low | Paths like `default.css` → `file:///path/to/MacSurf/Resources/CSS` |
| `mimetype` | Get MIME type for file path | `fetch_mimetype()`, similar to filetype but for arbitrary paths | Same extension-to-MIME table | Low | |
| `get_resource_data` | Load built-in resource to memory | Optional; not used in RISC OS | Optional; can embed resources or skip | Low | |
| `release_resource_data` | Free loaded resource | Optional | Optional | Low | |
| `socket_open` | Open socket (for fetch layer) | Optional; defaults to `socket()` | `OTOpenEndpoint()`, but MacSurf uses HTTP proxy, so this may not be needed | Low | cURL or proxy handles all networking |
| `socket_close` | Close socket | Optional; defaults to `close()` | `OTCloseProvider()` | Low | |

### gui_clipboard_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `get` | Get text from system clipboard | RISC OS data request protocol (`wimp_claim_entity`) → reads clipboard buffer | `GetScrap()` with `'TEXT'` type | Low | Get scrap handle, read text data, convert to UTF-8 |
| `set` | Put styled text on system clipboard | RISC OS claim entity + write to clipboard buffer | `ZeroScrap()` + `PutScrap()` with `'TEXT'` type | Low | Convert UTF-8 to MacRoman before `PutScrap()`. Style info ignored for v1 |

### gui_utf8_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `utf8_to_local` | Convert UTF-8 string to local encoding | UCS lookup tables in `ucstables.c` for Latin1/Latin2 etc. | `TECConvertText()` with TextEncoding Converter, UTF-8 to MacRoman/MacJapanese/etc. | Medium | Must create `TECObjectRef` for UTF-8 → `kTextEncodingMacRoman`. Cache the converter |
| `local_to_utf8` | Convert local encoding to UTF-8 | Reverse UCS tables | `TECConvertText()`, MacRoman to UTF-8 | Medium | Same TEC infrastructure, reverse direction |

### gui_download_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `create` | Create download progress window | Creates RISC OS save dialog window; shows file icon + progress bar | `CreateNewWindow()` with progress bar control + filename display | Medium | Need save-as dialog for destination, then progress window |
| `data` | Receive chunk of download data | `xosgbpb_writew()` to file; updates progress bar | `FSWrite()` to open file fork; update progress control | Low | |
| `error` | Report download error | Updates window status text with error | `StandardAlert()` or update window status | Low | |
| `done` | Download complete | Sets status to complete; enables open/save actions | Close progress window or update to "complete" state | Low | |

### core_window_table

Used for ancillary windows (cookies viewer, history, bookmarks). These use the same pattern as the main browser window but for simpler tree/list views.

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `invalidate` | Mark region for redraw | `xwimp_force_redraw()` on core window | `InvalWindowRect()` | Low | |
| `set_extent` | Set scrollable content size | `xwimp_set_extent()` | `SetControlMaximum()` on scroll bars to reflect content size | Low | |
| `set_scroll` | Set scroll position | `xwimp_open_window()` with modified scroll | `SetControlValue()` on scroll bars | Low | |
| `get_scroll` | Get scroll position | `xwimp_get_window_state()` | `GetControlValue()` from scroll bars | Low | |
| `get_dimensions` | Get visible area dimensions | `xwimp_get_window_state()`, compute visible area | `GetWindowBounds(kWindowContentRgn)` | Low | |
| `drag_status` | Update drag state cursor | Change pointer based on drag type | `SetCursor()` based on drag type | Low | |

### gui_search_table

| NetSurf Function | Purpose | RISC OS Implementation | Mac OS 9 Carbon/Toolbox Equivalent | Complexity | Notes |
|---|---|---|---|---|---|
| `status` | Report search found/not-found | Updates search dialog icon appearance | Update search dialog StaticText or icon | Low | |
| `hourglass` | Show/hide busy indicator | `xhourglass_on()` / `xhourglass_off()` | `SetCursor(*GetCursor(watchCursor))` / `InitCursor()` | Low | |
| `add_recent` | Add term to recent searches list | Adds to popup menu in search dialog | Add to popup menu control in search dialog | Low | |
| `forward_state` | Enable/disable "find next" button | Grey/ungrey dialog icon | `ActivateControl()` / `DeactivateControl()` on button | Low | |
| `back_state` | Enable/disable "find previous" button | Grey/ungrey dialog icon | `ActivateControl()` / `DeactivateControl()` on button | Low | |

---

## Section 2, POSIX Replacement Plan

### 2.1 iconv → TextEncoding Converter API

**POSIX:** `iconv_open()`, `iconv()`, `iconv_close()`, used in `utils/utf8.c` and `libparserutils/src/input/filter.c` for charset conversion (e.g., UTF-8 ↔ ISO-8859-1, Shift_JIS, etc.).

**Mac OS 9 Replacement:** TextEncoding Converter (TEC), available since Mac OS 8.5.

| POSIX Call | Mac OS 9 Equivalent | Notes |
|---|---|---|
| `iconv_open(to, from)` | `TECCreateConverter(&tec, from_enc, to_enc)` | Encoding constants from `<TextEncodingConverter.h>`. Map IANA charset names to `TextEncoding` values via `TECGetTextEncodingFromInternetName()` |
| `iconv(cd, inbuf, inleft, outbuf, outleft)` | `TECConvertText(tec, inbuf, inlen, &inread, outbuf, outlen, &outwritten)` | Handles partial conversions. Returns `kTECPartialCharErr` for incomplete input, same semantics as `E2BIG`/`EINVAL` |
| `iconv_close(cd)` | `TECDisposeConverter(tec)` | |

**Strategy:** Create a `mac_iconv.c` shim that wraps TEC with the same interface as NetSurf's `utils/utf8.c` expects. Cache `TECObjectRef` instances per encoding pair.

**libparserutils note:** The filter in `libparserutils/src/input/filter.c` also uses iconv. This must be patched with the same TEC wrapper, or libparserutils must be compiled with a custom filter backend.

### 2.2 mmap / munmap → FSRead + Custom Buffering

**POSIX:** `mmap()` and `munmap()`, used in `content/fetchers/file/file.c` (file fetcher) and libhubbub perf tests.

**Mac OS 9 Replacement:** No memory-mapped file I/O available.

| POSIX Call | Mac OS 9 Equivalent | Notes |
|---|---|---|
| `mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)` | `FSOpenFork()` + `FSAllocateFork()` + `FSReadFork()` into `malloc`'d buffer | Allocate buffer, read entire file. For large files, use chunked reads |
| `munmap(ptr, size)` | `free(ptr)` | |

**Strategy:** The file fetcher reads files to serve as `file:///` URLs. Replace `mmap` with a simple `FSReadFork()` into a heap buffer. Memory cost is bounded because file:/// content is typically small (local HTML, CSS, images). The libhubbub usage is test-only and not needed for production.

### 2.3 opendir / readdir / closedir → File Manager Iteration

**POSIX:** `opendir()`, `readdir()`, `closedir()`, used in `utils/utils.c`, `utils/file.c`, and several frontends for directory traversal.

**Mac OS 9 Replacement:**

| POSIX Call | Mac OS 9 Equivalent | Notes |
|---|---|---|
| `opendir(path)` | `FSOpenIterator(fsRef, kFSIterateFlat, &iterator)` | Obtain `FSRef` for directory first via `FSPathMakeRef()` or `FSMakeFSSpec()` + `FSpMakeFSRef()` |
| `readdir(dir)` | `FSGetCatalogInfoBulk(iterator, 1, &count, NULL, kFSCatInfoNodeFlags \| kFSCatInfoFinderInfo, &info, &ref, NULL, &name)` | Returns one entry per call (or batch for efficiency). Check `kFSNodeIsDirectoryMask` for subdirectories |
| `closedir(dir)` | `FSCloseIterator(iterator)` | |

**Strategy:** Write `mac_dirent.c` providing `opendir`/`readdir`/`closedir` wrappers over File Manager iterators. Carbon's File Manager (FSRef-based) is preferred over the older FSSpec APIs for long filename support.

### 2.4 fstatat / stat / fstat → FSGetCatalogInfo / FSpGetFInfo

**POSIX:** `fstatat()`, `stat()`, `fstat()`, used throughout for file existence checks, modification times, and size queries.

**Mac OS 9 Replacement:**

| POSIX Call | Mac OS 9 Equivalent | Notes |
|---|---|---|
| `stat(path, &buf)` | `FSPathMakeRef()` + `FSGetCatalogInfo(ref, kFSCatInfoDataSizes \| kFSCatInfoContentMod \| kFSCatInfoNodeFlags, &info, NULL, NULL, NULL)` | `info.dataLogicalSize` = file size; `info.contentModDate` = mtime; `info.nodeFlags & kFSNodeIsDirectoryMask` = is directory |
| `fstatat(dirfd, name, &buf, flags)` | Resolve path from parent `FSRef` via `FSMakeFSRefUniN()`, then `FSGetCatalogInfo()` | More cumbersome; must track parent FSRef |
| `fstat(fd, &buf)` | `FSGetForkSize()` for size; `FSGetCatalogInfo()` via fork's FSRef for other metadata | Need to maintain FSRef alongside file descriptor |

**Strategy:** Write `mac_stat.c` providing `stat()` / `fstat()` shims. For `fstatat()`, provide a wrapper that constructs a full path and calls the FSRef path. Most uses in NetSurf are simple "does this file exist and what's its size" checks.

### 2.5 gettimeofday → Microseconds / TickCount

**POSIX:** `gettimeofday()`, used for wall-clock timestamps in multiple frontends. `time()` for epoch seconds. `clock_gettime(CLOCK_MONOTONIC)` in duktape (not used, no JS).

**Mac OS 9 Replacement:**

| POSIX Call | Mac OS 9 Equivalent | Notes |
|---|---|---|
| `gettimeofday(&tv, NULL)` | `GetDateTime(&secs)` for seconds since 1904-01-01; `Microseconds(&us)` for high-res time | Convert Mac epoch (1904) to Unix epoch (1970) by subtracting 2082844800 seconds |
| `time(NULL)` | `GetDateTime(&secs) - 2082844800` | |
| `localtime()` / `gmtime()` | `DateTimeRec` via `SecondsToDate()` | Populate a `struct tm` from `DateTimeRec` fields |
| `mktime()` | `DateToSeconds()` | |
| `strftime()` | Manual formatting from `DateTimeRec` fields, or `IntlDateToString()` for locale-aware | NetSurf uses strftime for history timestamps, simple format strings only |
| `strptime()` | Manual parsing | Limited usage in NetSurf; already disabled on RISC OS via `HAVE_STRPTIME=0` |

**Strategy:** Write `mac_time.c` with thin wrappers. The RISC OS frontend already uses `os_read_monotonic_time()` for its scheduler (centisecond resolution). The Mac equivalent is `TickCount()` (1/60th second) for the scheduler, and `Microseconds()` for higher precision where needed.

### 2.6 signal → Not Needed

**POSIX:** `signal(SIGPIPE, SIG_IGN)` in `desktop/netsurf.c`; crash handlers (`SIGSEGV` etc.) in some frontends.

**Why not needed:**
- Mac OS 9 has no Unix-style signals. The cooperative multitasking model means there is no process-level signal delivery.
- `SIGPIPE` is a socket concern, on Mac OS 9, Open Transport returns errors inline (`kOTLookErr` etc.) rather than raising signals.
- Crash handling is done via `ExceptionInformation` and Macsbug or no-op. NetSurf's signal handlers are defensive, not functional.

**Strategy:** Define `HAVE_SIGPIPE=0` in `utils/config.h` under `__MACOS9__` (matching the RISC OS and Windows approach). No replacement code needed.

### 2.7 socket / select → Open Transport

**POSIX:** `socket()`, `select()`, `connect()`, `send()`, `recv()`, used minimally in NetSurf core (most networking goes through cURL). The `socket()` call appears in `desktop/gui_factory.c`. `select()` is used in the monkey frontend only.

**Mac OS 9 Replacement:** Open Transport (OT).

| POSIX Call | Open Transport Equivalent | Notes |
|---|---|---|
| `socket(AF_INET, SOCK_STREAM, 0)` | `OTOpenEndpoint(OTCreateConfiguration("tcp"), 0, NULL, &err)` | Returns `EndpointRef` |
| `connect(fd, addr, len)` | `OTConnect(ep, &sndCall, NULL)` after `OTBind(ep, NULL, NULL)` | **Must be async**, use `OTInstallNotifier()` and handle `T_CONNECT` in notifier callback |
| `send(fd, buf, len, 0)` | `OTSnd(ep, buf, len, 0)` | In async mode, handle `T_GODATA` event before sending |
| `recv(fd, buf, len, 0)` | `OTRcv(ep, buf, len, &flags)` | In async mode, handle `T_DATA` event in notifier |
| `select(nfds, &rfds, &wfds, NULL, &tv)` | `OTInstallNotifier()`, event-driven, no polling | OT is inherently event-driven. Schedule checks from WaitNextEvent loop via notifier callbacks |
| `close(fd)` | `OTCloseProvider(ep)` | |
| `setsockopt(fd, SOL_SOCKET, ...)` | `OTOptionManagement(ep, &req, &ret)` | |

**Critical constraint:** Every OT call must be asynchronous per CLAUDE.md, never block the cooperative event loop. Use `OTInstallNotifier()` with a notifier function, and process OT events during the WaitNextEvent idle loop.

**Strategy:** Since MacSurf uses a proxy for all HTTPS, the browser only needs plain HTTP over TCP. Implement a minimal async OT HTTP client:
1. `OTOpenEndpoint()` with async notifier
2. `OTBind()` → `OTConnect()` to proxy
3. Send HTTP request via `OTSnd()` on `T_GODATA`
4. Receive response via `OTRcv()` on `T_DATA`
5. Parse and feed data to NetSurf's fetch completion callbacks

This replaces cURL entirely for the Mac OS 9 build.

### 2.8 Other POSIX Calls

| POSIX Call | Where Used | Mac OS 9 Replacement | Notes |
|---|---|---|---|
| `access(path, mode)` | Multiple files, permission checks | `FSGetCatalogInfo()`, check `kFSCatInfoNodeFlags` | Mac OS 9 has no Unix permissions; just check file existence |
| `realpath(path, buf)` | `utils/filepath.c` | `FSResolveAliasFile()` + `FSRefMakePath()` | Resolves aliases (Mac equivalent of symlinks) |
| `rmdir(path)` | `utils/file.c` | `FSDeleteObject()` after `FSPathMakeRef()` | Only works on empty directories |
| `unlink(path)` | Not directly used, but implied | `FSDeleteObject()` | |
| `mkdir(path, mode)` | Implied by file operations | `FSCreateDirectoryUnicode()` | |

---

## Section 3, Risk Register

Ordered by difficulty, hardest first.

### 1. Open Transport Async Networking, CRITICAL

**Risk:** All networking must be asynchronous to avoid blocking the cooperative event loop. OT's async API is complex, poorly documented, and notoriously buggy on certain OS 9 versions. Incorrect async handling causes system-wide hangs.

**Difficulty:** High

**Mitigation:** Start with the simplest possible OT usage, a single async TCP endpoint per fetch. Use `OTInstallNotifier()` with a state machine (IDLE → CONNECTING → SENDING → RECEIVING → DONE). Test on multiple OS 9 versions (9.1, 9.2.2). The proxy architecture simplifies this enormously: we only need HTTP/1.0 plain text to one host, not arbitrary HTTPS to many.

### 2. Bezier Path Rendering, HIGH

**Risk:** QuickDraw has no native cubic bezier curve support. The `path` plotter function handles SVG and CSS-drawn paths with bezier curves and arbitrary transforms. Manual curve flattening (de Casteljau subdivision) must be implemented from scratch.

**Difficulty:** High

**Mitigation:** Implement adaptive de Casteljau subdivision that flattens bezier curves to polylines within a configurable tolerance (1-2 pixel error). This is a well-known algorithm. Most web pages use beziers rarely (mostly for SVG icons). If too complex initially, return `NSERROR_OK` without rendering, the page will display with missing decorative paths but remain functional.

### 3. iconv / Character Encoding, HIGH

**Risk:** iconv is used deep in two codebases, NetSurf core (`utils/utf8.c`) and libparserutils (`src/input/filter.c`). The TextEncoding Converter API has a different interface and different error semantics. Charset name mapping (IANA names to TextEncoding constants) must be comprehensive. Incorrect encoding conversion will produce garbled text on every page.

**Difficulty:** High

**Mitigation:** Build a `mac_iconv.c` shim wrapping TEC. Prioritize the 10 most common web encodings: UTF-8, ISO-8859-1, Windows-1252, Shift_JIS, EUC-JP, GB2312, EUC-KR, ISO-8859-15, UTF-16, US-ASCII. Use `TECGetTextEncodingFromInternetName()` for name mapping, it handles most IANA names natively. Write a test harness that round-trips strings through the shim and compares against known-good iconv output.

### 4. Font Measurement and Rendering, MEDIUM-HIGH

**Risk:** The `gui_layout_table` (width, position, split) drives all text layout. Inaccurate font measurement breaks page layout globally, text overflows, columns misalign, line breaks occur in wrong places. QuickDraw font metrics may differ subtly from CSS expectations. UTF-8 strings must be converted to Mac encoding before measurement.

**Difficulty:** Medium-High

**Mitigation:** Use `TextWidth()` / `CharWidth()` for measurement after setting `TextFont()` + `TextSize()` + `TextFace()`. Map CSS font families to Mac system fonts: sans-serif → Geneva, serif → Times, monospace → Monaco, cursive → Zapf Chancery, fantasy → Gadget or Charcoal. For `position()`, implement binary search over `TextWidth()` to find character offset. For `split()`, use same approach as RISC OS, find pixel split, walk back to space. Test extensively against known page layouts.

### 5. Bitmap Alpha Compositing, MEDIUM-HIGH

**Risk:** QuickDraw on Mac OS 9 has no native alpha blending. PNG images with transparency and CSS semi-transparent elements require software alpha compositing. The `bitmap` plotter must composite RGBA bitmaps onto the window content correctly.

**Difficulty:** Medium-High

**Mitigation:** Implement software alpha compositing in the bitmap plotter: for each pixel, blend `src_alpha * src_color + (1 - src_alpha) * dst_color`. Read destination pixels from the window's GrafPort, blend, write back. For opaque bitmaps (common case), use `CopyBits()` directly, no blending needed. Optimize with a fast-path check: if `bitmap->opaque`, use `CopyBits(srcCopy)`. Consider pre-multiplied alpha to reduce per-pixel math.

### 6. Memory Pressure on 64MB Systems, MEDIUM

**Risk:** Mac OS 9 target is 64MB minimum. NetSurf's backing store, bitmap cache, and parsed DOM trees can consume significant memory. No virtual memory paging to rely on. Running out of memory on OS 9 typically crashes the system.

**Difficulty:** Medium

**Mitigation:** Configure NetSurf's cache sizes conservatively at compile time: bitmap cache 2MB, content cache 4MB, backing store 8MB. Implement `bitmap_modified` to eagerly free decoded bitmaps that can be re-fetched. Monitor `FreeMem()` / `TempFreeMem()` and trigger cache eviction when free memory drops below 8MB. Disable backing store entirely if memory is critically low. Test with 64MB and 32MB partitions.

### 7. File I/O Abstraction Layer, MEDIUM

**Risk:** NetSurf core uses POSIX file I/O throughout, `open()`, `read()`, `write()`, `close()`, `stat()`, `opendir()`, `readdir()`. This is a broad surface area. Incomplete or buggy replacement will cause silent data corruption (wrong cache reads, broken cookies/history persistence).

**Difficulty:** Medium

**Mitigation:** Build `mac_posix_io.c` with shims for all POSIX file calls, wrapping Carbon File Manager (FSRef API). Implement in order of importance: (1) `stat`/`fstat`, most used, (2) `open`/`close`/`read`/`write`, (3) `opendir`/`readdir`/`closedir`, (4) `mkdir`/`rmdir`/`unlink`. Test by running the NetSurf backing store and URL database save/load cycles. All path handling must account for Mac `:` separators vs Unix `/`, use `FSPathMakeRef()` which accepts Unix-style paths under Carbon.

### 8. CodeWarrior 8 C99 Compatibility, MEDIUM

**Risk:** While the audit confirms C99 throughout, CodeWarrior 8's C99 support has known gaps: no `_Bool` keyword (uses `bool` from `<stdbool.h>` which CW provides), potential issues with variable-length arrays (VLAs), `designated initializers` partially supported, `long long` works but `<stdint.h>` may need supplementing.

**Difficulty:** Medium

**Mitigation:** Do an early test compile of each library (libparserutils, libwapcaplet, libcss, libhubbub, libdom) with CodeWarrior to identify specific failures. Known issues: VLAs (used in `plotters.c` `ro_plot_polygon`, the `int path[n * 3 + 2]`) must be replaced with `malloc`. Designated initializers in struct tables (`.create = func`), CW8 supports these. Run a focused test compile before writing any frontend code.

### 9. WaitNextEvent Integration, MEDIUM

**Risk:** NetSurf's core expects the frontend to call `schedule_run()` and process fetches periodically. The Mac event loop must correctly interleave WaitNextEvent handling, scheduled callback dispatch, and network I/O without starving any subsystem.

**Difficulty:** Medium

**Mitigation:** Follow the RISC OS pattern exactly:
1. `WaitNextEvent()` with appropriate sleep time (minimum of next scheduled callback time and OT event readiness)
2. Process Mac events (mouse, keyboard, update, activate)
3. Call `schedule_run()` to dispatch due callbacks
4. Check OT notifier flags and process pending network data
5. Loop

Use `TickCount()` for scheduler timing. Set WaitNextEvent sleep to 1 tick (1/60s) when active fetches are in progress, 15 ticks when idle.

### 10. Cross-Compilation Toolchain, LOW-MEDIUM

**Risk:** Building a PPC Mac OS 9 binary from Linux requires either CodeWarrior on a real/emulated Mac, or a cross-compilation toolchain (GCC PPC with Mac OS 9 headers and libraries). No well-maintained cross-compiler exists for this target.

**Difficulty:** Low-Medium

**Mitigation:** Two paths, pursue in parallel:
- **Path A (primary):** Use SheepShaver or QEMU with Mac OS 9 and CodeWarrior 8. Develop on Linux, transfer source via shared folder, compile on emulated Mac. Slow but proven.
- **Path B (stretch):** Use `powerpc-apple-macos-gcc` (Retro68 or similar) with MPW/Universal Headers. This is faster iteration but requires significant toolchain setup. Only pursue if Path A proves too slow for development iteration.
