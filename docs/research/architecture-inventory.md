# MacSurf Architecture Inventory

## Purpose

A snapshot of what currently exists in the MacSurf repo and on the production
proxy host as of 2026-04-11. **No decisions, no recommendations, no plans ,
only what is in place today.** Numbers, line counts, and code paths are taken
directly from the working tree at HEAD (`master`, commit ahead of
`v0.1.0-first-fetch`).

---

## 1. Repository top-level layout

```
macsurf/
├── browser/netsurf/        NetSurf engine fork (frontends/macos9/ is the new code)
├── proxy/                  Go TLS-stripping proxy (5 files)
├── docs/                   Build, deploy, status, and research notes
├── CLAUDE.md               Project instructions
├── README.md               (top-level)
├── build-error-report.md   Dated CW8 build snapshot
├── fixes.zip               Latest delivery zip for the OS 9 dev box
└── MEMORY.md (in ~/.claude) Cross-session memory index
```

---

## 2. The Go proxy

### 2.1 Source files

`proxy/` contains exactly **five** Go files plus a Dockerfile and systemd unit:

| File | Lines | Purpose |
|---|---:|---|
| [`proxy/main.go`](../../proxy/main.go) | 65 | Entry point: flag parsing, server lifecycle, signal handling |
| [`proxy/proxy.go`](../../proxy/proxy.go) | 111 | `Proxy.ServeHTTP` dispatcher, plain-HTTP and CONNECT handlers, byte pump |
| [`proxy/auth.go`](../../proxy/auth.go) | 41 | `Credentials` struct, `Proxy-Authorization` header parsing |
| [`proxy/Dockerfile`](../../proxy/Dockerfile) | 9 | Two-stage `golang:1.23` → `scratch`, copies CA bundle, statically linked |
| [`proxy/macsurf-proxy.service`](../../proxy/macsurf-proxy.service) | 27 | systemd unit |

`go.mod` declares module `macsurf-proxy`, Go 1.25.4, **zero external
dependencies**, stdlib only.

### 2.2 Entry point and request flow

`main()` ([proxy/main.go:16](../../proxy/main.go#L16)):

1. Parses two flags: `--port` (default `8765`) and `--auth` (default empty ,
   no authentication)
2. If `--auth` is set, calls `ParseCredentials("user:password")` and stores
   the result on `Proxy.Auth`
3. Constructs an `http.Server` with `ReadTimeout: 30s` and the `Proxy` value
   as `Handler`
4. Goroutine: `server.ListenAndServe()`
5. Main goroutine: blocks on `signal.NotifyContext` for `SIGINT` / `SIGTERM`,
   then `server.Shutdown(5s)`

`Proxy.ServeHTTP()` ([proxy/proxy.go:15](../../proxy/proxy.go#L15)):

1. If `Auth != nil` and `Credentials.Check(r)` returns false, send `407`
   with `Proxy-Authenticate: Basic realm="macsurf-proxy"`
2. If `r.Method == "CONNECT"` → `handleConnect`
3. Otherwise → `handleHTTP`

`handleHTTP()` ([proxy/proxy.go:29](../../proxy/proxy.go#L29)):

1. Reject if `r.URL.Host == ""` → 400
2. Build a new `http.Request` with the same method, body, and full URL
3. Copy all headers via `copyHeaders`
4. Strip `Proxy-Authorization` and `Proxy-Connection` from the outbound
5. `http.DefaultTransport.RoundTrip(outReq)`, Go's stdlib transport handles
   the upstream request, including TLS for `https://` URLs
6. Copy response headers, status, and body back to the client

`handleConnect()` ([proxy/proxy.go:56](../../proxy/proxy.go#L56)):

1. `net.DialTimeout("tcp", r.Host, 10s)` to the upstream
2. Hijack the client connection via `http.Hijacker`
3. Reply `200`, then spawn two goroutines pumping bytes between the two
   connections via `transfer()`
4. `transfer()` uses a 32KB buffer and a 10-minute idle timeout per direction

### 2.3 What the proxy does and does not do

What it does:

- Forward `GET`/`POST`/etc. plain-HTTP requests to upstream, including
  `https://` URLs (Go's `DefaultTransport` performs the TLS handshake)
- Tunnel `CONNECT` for end-to-end TLS (used by clients that want to do TLS
  themselves)
- Strip `Proxy-Authorization` and `Proxy-Connection` from the outbound
  request
- Optional `Proxy-Authorization: Basic` enforcement when `--auth` is set
- Honor `SIGINT` / `SIGTERM` for graceful shutdown

What it does not do, based on what is in the source:

- No URL rewriting
- No HTML rewriting (no body inspection at all)
- No image transformation, downscaling, or transcoding
- No content-type filtering or modification
- No on-disk caching (no cache directory, no key/value store)
- No logging beyond `log.Printf` startup line and Go default error logging
- No access list / IP filtering
- No request rate limiting
- No CONNECT host filtering
- No `Via` header rewriting
- No `X-Forwarded-For` injection

### 2.4 Production deployment (Hetzner)

Verified directly from the host this repo is checked out on
(`Ubuntu-2404-noble-amd64-base`, public IP `116.202.231.103`):

- **Service:** `macsurf-proxy.service` (systemd, **active running** since
  2026-04-07 23:07:28 CEST, uptime 3 days at inventory time)
- **Binary:** `/usr/local/bin/macsurf-proxy`
- **Run user / group:** `macsurf` / `macsurf` (created for this service)
- **Args:** `--port 8765`, sourced from
  `EnvironmentFile=/etc/macsurf-proxy/env` which contains
  `MACSURF_PROXY_ARGS=--port 8765`
- **Resource use:** PID 2348560, 13 tasks, 5.3 MB RSS (peak 8.4 MB), 3.5
  CPU-seconds total over 3 days
- **Listening on:** `0.0.0.0:8765` (TCP, IPv4 + IPv6), confirmed via
  `ss -tlnp`
- **Service hardening:** `NoNewPrivileges`, `ProtectSystem=strict`,
  `ProtectHome`, `PrivateTmp`, `PrivateDevices`
- **Restart policy:** `Restart=on-failure`, `RestartSec=5`
- **Logging:** journald, `SyslogIdentifier=macsurf-proxy`, no recent log
  entries (the proxy is silent during normal operation)

The proxy port is **not** behind nginx. The Mac connects directly to
`116.202.231.103:8765` over the public internet (this is the value
hardcoded in [macos9_fetch.c](../../browser/netsurf/frontends/macos9/macos9_fetch.c)).
nginx on the same host is running for unrelated services
(`clarogrid.com`, `cloud.mp.ls`, etc., none of them MacSurf-related).

Other ports listening on the host that are unrelated to MacSurf but worth
noting for context: `:80`, `:443`, `:25`, `:143`, `:465`, `:587`, `:993`,
`:4190` (mail/web stack), plus various local-only Postgres/Redis/Node
services on `127.0.0.1`.

---

## 3. The Mac OS 9 frontend rendering pipeline

### 3.1 What `macos9_fetch_url()` does, start to finish

[browser/netsurf/frontends/macos9/macos9_fetch.c](../../browser/netsurf/frontends/macos9/macos9_fetch.c) ,
the function called from `window.c` when the user enters a URL or clicks
a navigation button. Reading top to bottom:

1. **Includes**, `<OpenTransport.h>`, `<OpenTptInternet.h>`, `<Threads.h>`,
   `extern OTClientContextPtr macos9_ot_context` (defined in `main.c`)
2. **Constants**, `MACSURF_PROXY_HOST = "116.202.231.103"`,
   `MACSURF_PROXY_PORT = 8765`
3. **`macos9_fetch_filetype()`**, extension-to-MIME-type sniffer used by
   `macos9_fetch_table` (called by NetSurf's core for local files;
   irrelevant for HTTP)
4. **`macos9_fetch_get_resource_url()`**, returns `NULL` (no resource:
   scheme support)
5. **`macos9_fetch_mimetype()`**, `strdup` of the filetype lookup
6. **`yield_notifier()`**, `pascal void` callback installed on the OT
   endpoint. On `kOTSyncIdleEvent` calls `YieldToAnyThread()`. This is
   the cooperative-multitasking yield point during blocking OT calls.

7. **`macos9_fetch_url(url, gw, callback)`**, the actual fetch:
   - `OTCreateConfiguration("tcp")`, uses literal `"tcp"`, not the
     `kTCPName` macro
   - Bail with `"OT config failed"` callback if NULL
   - `OTOpenEndpointInContext(cfg, 0, NULL, &err, macos9_ot_context)`
   - Bail with `"OT open failed"` callback on error
   - `NewOTNotifyUPP(yield_notifier)` for the UPP
   - `OTSetSynchronous(ep)`
   - `OTSetBlocking(ep)`
   - `OTInstallNotifier(ep, notifyUPP, NULL)`
   - `OTUseSyncIdleEvents(ep, true)`
   - `OTBind(ep, NULL, NULL)`, both args NULL
   - `sprintf(proxy_addr, "%s:%d", host, port)` then
     `OTInitDNSAddress(&dns_addr, proxy_addr)`
   - `OTConnect(ep, &snd_call, NULL)`, sync, blocks, yields via notifier
   - Build a fixed `GET %s HTTP/1.0\r\nUser-Agent: MacSurf/0.1\r\nConnection: close\r\n\r\n`
     request
   - `OTSnd(ep, request, req_len, 0)`
   - `NewPtr(MACSURF_CONTENT_MAX + 4)`, allocate receive buffer
   - Loop: `OTRcv(ep, ...)` until 0 bytes or buffer full
   - Find `\r\n\r\n` in the received bytes; treat everything after as the
     body
   - Call back with `(gw, body, body_len, 0)`
   - `DisposePtr(buf)`
   - `OTSndOrderlyDisconnect(ep)`
   - `OTCloseProvider(ep)`
   - `DisposeOTNotifyUPP(notifyUPP)`

8. **`macos9_strip_html(src, src_len, dst, dst_cap)`**, regex-level tag
   stripper. Tracks `in_tag` / `in_script` / `in_style`, decodes
   `&amp;`/`&lt;`/`&gt;`/`&quot;`/`&apos;`/`&nbsp;`, collapses whitespace,
   inserts `\n` at block-element close tags. Output is plain text.

9. **`macos9_word_wrap(text, text_len, line_offsets, line_lengths, max_lines, max_chars_per_line)`**
  , fixed-width word wrap. Honors existing `\n` as forced breaks. Breaks
   on space when possible, hard-breaks otherwise. Populates two parallel
   arrays of offsets and lengths into the source buffer.

10. **`macos9_ot_init()` / `macos9_ot_get_error()`**, both vestigial
    stubs returning 0 / NULL. Not called from anywhere.

11. **`macos9_fetch_table`**, filled with the type sniffers from above.
    The HTTP-fetcher field is `NULL`. NetSurf core's fetch dispatch sees
    this table but cannot route through it for HTTP.

### 3.2 What runs from a click to "text on screen"

1. User clicks Back/Forward/Reload/Home or hits Return in the URL bar
2. [window.c](../../browser/netsurf/frontends/macos9/window.c) sets
   `gw->fetch_pending = true`
3. The next iteration of `macos9_poll()` in
   [main.c](../../browser/netsurf/frontends/macos9/main.c) runs the
   pending fetch synchronously: `macos9_fetch_url(gw->url, gw, macos9_fetch_callback)`
4. `macos9_fetch_callback` (in main.c) receives raw HTTP body:
   - `macos9_strip_html()` into `gw->content`
   - `gw->content_len` updated, status set to "Ready"
   - `macos9_window_invalidate_content(gw)` to queue a redraw
5. `window.c::macos9_window_rewrap()` re-runs `macos9_word_wrap()` over the
   new content using the current content-rect width
6. `macos9_window_update_scrollbar()` updates the scrollbar range from
   line count
7. On the next `updateEvt`, `macos9_handle_update()` in `main.c` draws the
   visible lines via `MoveTo` + `DrawText` from `gw->line_offsets[]` /
   `gw->line_lengths[]`

**The NetSurf core (`browser_window`, `hlcache_handle`, `content`,
plotters) is not involved in this pipeline at all.** The frontend hands a
URL to OT, parses HTML by hand, and draws plain text.

---

## 4. All stub files in the frontend

### 4.1 Stub files on disk

Seven `*_stub.c` files exist in [browser/netsurf/frontends/macos9/](../../browser/netsurf/frontends/macos9/):

| File | Funcs | What it stubs out |
|---|---:|---|
| [`misc_stub.c`](../../browser/netsurf/frontends/macos9/misc_stub.c) | 32 | The big catch-all: `image_cache_init`, `image_cache_fini`, `nscss_init`, `html_init`, `image_init`, `textplain_init`, `fetcher_init`, `ns_system_colour_init`, `nscolour_update`, `page_info_init`, `page_info_fini`, `dom_namespace_initialise`, `dom_namespace_finalise`, `idna_encode`, `save_pdf`, `html_get_id_offset`, `hotlist_update_url`, `global_history_add`, `content_textsearch_destroy`, `search_web_init`, `search_web_finalise`, `cert_chain_alloc`, `cert_chain_free`, `cert_chain_to_query`, `cert_chain_dup`, `download_context_create`, `download_context_destroy`, `lwc_iterate_strings`, `free_user_agent_string`, `fetch_abort` |
| [`fetch_stub.c`](../../browser/netsurf/frontends/macos9/fetch_stub.c) | 9 | `fetch_start`, `fetch_can_fetch`, `fetch_http_code`, `fetch_quit`, `fetcher_quit`, `fetch_multipart_data_new_kv`, `fetch_multipart_data_find`, `fetch_multipart_data_clone`, `fetch_multipart_data_destroy` |
| [`http_stub.c`](../../browser/netsurf/frontends/macos9/http_stub.c) | 16 | `http_parse_content_type` + destroy, `http_parse_cache_control` + destroy, `http_parse_content_disposition` + destroy, `http_parse_www_authenticate` + destroy, `http_parse_strict_transport_security` + destroy, plus 6 helpers |
| [`js_stub.c`](../../browser/netsurf/frontends/macos9/js_stub.c) | 11 | `js_initialise`, `js_finalise`, `js_newheap`, `js_destroyheap`, `js_newthread`, `js_closethread`, `js_destroythread`, `js_handle_new_element`, `js_event_cleanup`, plus 2 |
| [`browser_history_stub.c`](../../browser/netsurf/frontends/macos9/browser_history_stub.c) | 18 | `browser_window_history_create`/`destroy`/`add`/`update`/`get_scroll`, `scrollbar_create`/`destroy`/`set`/`get_offset`, `browser_window_place_caret`/`remove_caret`, `browser_window_create_iframes`, `browser_window_recalculate_iframes`, `browser_window_invalidate_iframe`, `browser_window_create_frameset`, `browser_window_recalculate_frameset`, `browser_window_destroy_iframes`, `browser_window_handle_scrollbars` |
| [`corestrings_stub.c`](../../browser/netsurf/frontends/macos9/corestrings_stub.c) | 2 | **Not a real stub**, actually implements `corestrings_init` / `corestrings_fini` |
| [`lwc_stub.c`](../../browser/netsurf/frontends/macos9/lwc_stub.c) | 3 | **Not a real stub**, actually implements `lwc_intern_string`, `lwc_string_destroy`, plus one helper |

### 4.2 Stub files in the CW8 project

**None.** Verified by grepping `MacSurf.mcp` for `stub`, zero matches.
None of the seven `*_stub.c` files are referenced as `<FILEREF>` entries
in the project. They exist on disk but are not part of the build.

This means symbols those stubs declare must be provided by some other
file at link time (or the link fails). The likely candidates: the real
NetSurf source files that **are** in the project (see §6.2 below), MSL,
or CarbonLib. This inventory does not investigate which symbols actually
resolve where, that is a separate question.

### 4.3 Header-only stubs (subdirectory shims)

Stub headers exist in subdirectories under
[browser/netsurf/frontends/macos9/](../../browser/netsurf/frontends/macos9/)
to satisfy `#include` lines in core NetSurf source. These are header-only
no `.c` files for these libraries are in the tree:

| Directory | Header(s) | Defers |
|---|---|---|
| `libwapcaplet/` | `libwapcaplet.h` | The string-interning library, actual implementation is in `lwc_stub.c` |
| `libcss/` | `libcss.h` | Entire CSS parser/cascade |
| `dom/` | `dom.h` | Entire DOM library |
| `css/` | `css.h`, `utils.h` | NetSurf's CSS wrappers |
| `html/` | `html.h`, `form_internal.h` | NetSurf's HTML internals |
| `image/` | `image.h`, `image_cache.h` | Image content handler |
| `text/` | `textplain.h` | Plain-text content handler |
| `javascript/` | `js.h` | JS interface |
| `parserutils/` | (charset/utf8 stubs, see [macsurf_prefix.h](../../browser/netsurf/frontends/macos9/macsurf_prefix.h)) | parserutils library |
| `nsutils/` | `endian.h`, `time.h`, `base64.h`, `unistd.h` | nsutils helper library |
| `arpa/`, `netinet/`, `sys/` | `inet.h`, `in.h`, `select.h`, `socket.h`, `time.h`, `types.h`, `utsname.h` | POSIX network and system headers |
| `shims/` | `iconv.h`, `zlib.h`, `stat.h`, `dirent.h`, `inttypes.h`, `mac_*.h`, `mac_*.c`, `stdint.h` | POSIX functionality (these have real `.c` implementations) |

The five `shims/mac_*.c` files **are** in the CW8 project: `mac_iconv.c`,
`mac_file_io.c`, `mac_stat.c`, `mac_dirent.c`, `mac_time.c`.

---

## 5. Frontend source files (Mac-specific code)

[browser/netsurf/frontends/macos9/](../../browser/netsurf/frontends/macos9/) contains
both files in the project and files that exist alongside it. From the
project file list:

**In the CW8 project (12 frontend `.c` files):**

| File | Purpose |
|---|---|
| `main.c` | Entry point, Toolbox init, OT lifecycle, `WaitNextEvent` loop, event dispatch, fetch callback, content drawing |
| `window.c` | Carbon window with toolbar (Back/Fwd/Reload/Home), URL bar, scrollbar; navigation/history/wrap orchestration |
| `macos9_fetch.c` | OT-backed HTTP fetch + HTML strip + word wrap |
| `plotters.c` | NetSurf plotter callbacks (all currently no-op stubs) |
| `font.c` | Font/text-measurement layer |
| `macos9_bitmap.c` | Bitmap allocator/destroyer for NetSurf bitmap table |
| `clipboard.c` | Clipboard table stubs |
| `macos9_utf8.c` | UTF-8 conversion table |
| `macos9_download.c` | Download table stubs |
| `misc.c` | Misc table glue |
| `schedule.c` | NetSurf cooperative scheduler (`macos9_schedule`, `macos9_schedule_run`) |
| (5 shims listed above under §4.3) | POSIX shims |

**On disk but NOT in the CW8 project:** the seven stub files
(`misc_stub.c`, `fetch_stub.c`, `http_stub.c`, `js_stub.c`,
`browser_history_stub.c`, `corestrings_stub.c`, `lwc_stub.c`).

### 5.1 Resource files

| File | Size | Format |
|---|---:|---|
| [`MacSurf.r`](../../browser/netsurf/frontends/macos9/MacSurf.r) | 40 B | Rez source for the `'carb'` resource (kept on disk, not in git via .gitignore status) |
| [`MacSurf.rsrc`](../../browser/netsurf/frontends/macos9/MacSurf.rsrc) | 306 B | Pre-compiled binary Mac resource fork containing one `'carb'` resource ID 0, generated on Linux via Python |

`MacSurf.rsrc` is what the CW8 project links into the application's
resource fork at build time so CFM recognises the binary as a Carbon
fragment.

### 5.2 The CW8 project file itself

[`MacSurf.mcp`](../../browser/netsurf/frontends/macos9/MacSurf.mcp), XML
text, 753 lines. This is the new XML format that ships with CW8 / CW9, not
the binary `.mcp` of older CodeWarrior versions.

Project settings of note (from the XML):

- Target type: PowerPC application (`MWProject_PPC_type`)
- C++ disabled, C99 disabled (`MWFrontEnd_C_cplusplus`,
  `MWFrontEnd_C_c99`), strict C89
- Prefix file: [`macsurf_prefix.h`](../../browser/netsurf/frontends/macos9/macsurf_prefix.h)
- Linker generates a map file
- Pedantic warnings enabled
- Four `UserSearchPath` entries and two `SystemSearchPath` entries
- Processor: PowerPC, dead-code stripping enabled

---

## 6. What the CW8 project includes

### 6.1 Frontend (12 `.c` + 5 shims = 17 files)

See §5 above.

### 6.2 NetSurf core (10 `.c` files from upstream)

From `<FILEREF>` entries in [`MacSurf.mcp`](../../browser/netsurf/frontends/macos9/MacSurf.mcp):

| File | Subsystem |
|---|---|
| `utils/utils.c` | misc utilities |
| `utils/file.c` | file path utilities |
| `utils/filepath.c` | search path resolver |
| `utils/log.c` | logging |
| `utils/time.c` | time helpers |
| `utils/nsurl.c` | NetSurf URL parser |
| `utils/url.c` | URL utilities |
| `utils/utf8.c` | UTF-8 helpers |
| `utils/messages.c` | localized message table |
| `utils/nsoption.c` | options system |
| `content/llcache.c` | low-level cache |
| `content/fetch.c` | fetch dispatch (real `fetcher_init` lives here, not in the stub) |
| `content/fs_backing_store.c` | filesystem-backed cache store |
| `content/content.c` | content base class |
| `content/hlcache.c` | high-level cache |
| `content/mimesniff.c` | MIME sniffing |
| `content/urldb.c` | URL database |
| `desktop/browser.c` | browser context |
| `desktop/browser_window.c` | browser window logic |
| `desktop/netsurf.c` | top-level `netsurf_init` / `netsurf_exit` |
| `desktop/gui_factory.c` | `netsurf_register` validation |
| `desktop/plot_style.c` | plot style helpers |

This is **27 source files total** in the build (5 shims + 12 frontend +
22 NetSurf core). The `(39 .c files)` count in
[CLAUDE.md](../../CLAUDE.md) does not match the current `.mcp` ,
inventory shows 27.

### 6.3 Libraries linked

Three `<PATH>` entries that are libraries, not source files:

| Library | Path |
|---|---|
| `MSL C.Carbon.Lib` | `MacOS Support/Libraries/Runtime/Shared Support/MSL C.Carbon.Lib` |
| `MSL C.Carbon.Lib` | `MacOS Support/Libraries/MSL/MSL_C/MSL_Common/Lib/MSL C.Carbon.Lib` |
| `CarbonLib` | bare path (resolved against the access path) |

(`MSL C.Carbon.Lib` appears twice, once from each MSL location. CW8
resolves duplicates by access-path order.)

### 6.4 Access paths

User paths (4):

```
{Project}::patrick:macsurf-source Folder:
{Project}::patrick:macsurf-source Folder:browser:netsurf:
{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:
{Project}::patrick:macsurf-source Folder:browser:netsurf:frontends:macos9:shims:
```

System paths (2):

```
{Compiler}:MacOS Support:Universal:Interfaces:CIncludes:
{Compiler}:MacOS Support:MacHeaders:
```

CLAUDE.md lists more access paths than this; the live `.mcp` has been
trimmed to four user paths. Worth flagging.

### 6.5 Prefix file

[`macsurf_prefix.h`](../../browser/netsurf/frontends/macos9/macsurf_prefix.h)
defines:

- `__MACOS9__ 1`
- `WITHOUT_DUKTAPE 1`
- `NO_IPV6 1`
- `TARGET_API_MAC_CARBON 1`
- `#include <MacTypes.h>` (must be first)

---

## 7. Existing docs

### 7.1 Top-level docs

| File | One-line summary |
|---|---|
| [`docs/codewarrior-setup.md`](../codewarrior-setup.md) | Step-by-step guide for installing CodeWarrior 8 and building MacSurf on a real Power Mac |
| [`docs/deploying-proxy.md`](../deploying-proxy.md) | How to deploy the Go proxy as a single binary on a VPS or local machine |
| [`docs/proxy-test-results.md`](../proxy-test-results.md) | Snapshot of proxy curl tests after fixing `WriteTimeout` and Dockerfile issues (dated 2026-04-07) |
| [`docs/status.md`](../status.md) | Current project status, milestone progress, what works, libraries linked, file inventory, test environment |

### 7.2 Research docs

| File | One-line summary |
|---|---|
| [`carbonlib-ot-relationship.md`](carbonlib-ot-relationship.md) | Whether CarbonLib requires `RegisterAppearanceClient` before OT, answer: no, but related Carbon-app contract issues exist |
| [`classilla-carb-resource.md`](classilla-carb-resource.md) | Inspection of Classilla source for the `'carb'` Carbon-app marker resource |
| [`classilla-startup-sequence.md`](classilla-startup-sequence.md) | Classilla's full startup sequence under Carbon, every Toolbox call before networking |
| [`core-compile-attempt.md`](core-compile-attempt.md) | First syntax-check of NetSurf core `utils/` files with `__MACOS9__` defined |
| [`frontend-api-mapping.md`](frontend-api-mapping.md) | Maps every NetSurf frontend callback to its Mac OS 9 Toolbox equivalent based on the RISC OS reference |
| [`macsurf-main-c-audit.md`](macsurf-main-c-audit.md) | Audit of MacSurf's `main.c` for Carbon-app contract gaps |
| [`netsurf-audit.md`](netsurf-audit.md) | Audit of NetSurf and its dependency libraries for the MacSurf project |
| [`netsurf-core-integration.md`](netsurf-core-integration.md) | Map of `netsurf_init` / `browser_window_create` / `hlcache_handle` integration paths and required stub replacements |
| [`ot-carbon-prior-art.md`](ot-carbon-prior-art.md) | Prior art for HTTP-over-OT on OS 9: SSHeven, cy384/miscellany retro68 demo, Apple's `OTSimpleDownloadHTTP.c` |
| [`posix-portability.md`](posix-portability.md) | Comprehensive analysis of every POSIX dependency in NetSurf with shim sizing and ordering |
| [`scaffold-status.md`](scaffold-status.md) | Status of the initial frontend scaffold, files compile cleanly with no Toolbox calls |
| [`scheduler-status.md`](scheduler-status.md) | Cooperative-multitasking scheduler and `WaitNextEvent` loop status |
| [`shim-status.md`](shim-status.md) | Status of the POSIX shim layer in `frontends/macos9/shims/` |

`docs/research/architecture-inventory.md` is this file.

`docs/milestone-v0.1.0.md` does not exist, the v0.1.0 milestone report
was written in a chat session but never committed as a file.

---

## 8. Reference appendix, facts checked at inventory time

- Repo: `/home/patrick/Webs/macsurf` on host `Ubuntu-2404-noble-amd64-base`
- Public IP: `116.202.231.103` (this host is the proxy host)
- macsurf-proxy uptime: 3 days as of inventory
- macsurf-proxy memory: 5.3 MB RSS, 8.4 MB peak
- CW8 project file: `browser/netsurf/frontends/macos9/MacSurf.mcp`, 753 lines, XML format
- Frontend source files on disk in `frontends/macos9/`: 12 `.c` + 7 `*_stub.c`
- Frontend `.c` files in CW8 project: 12 (none of the stubs)
- NetSurf core `.c` files in CW8 project: 22 (utils, content, desktop)
- Shim `.c` files in CW8 project: 5 (`mac_iconv`, `mac_file_io`, `mac_stat`, `mac_dirent`, `mac_time`)
- Libraries in CW8 project: 3 entries (2× `MSL C.Carbon.Lib`, 1× `CarbonLib`)
- Total `.c` files in build: 27 (CLAUDE.md still says 39)
- `MacSurf.rsrc`: 306 bytes, contains one `'carb'` resource ID 0
- `corestrings_stub.c` and `lwc_stub.c`: name says "stub" but both contain real implementations
- Top-level docs in `docs/`: 4 files
- Research docs in `docs/research/`: 13 files (this inventory makes 14)
