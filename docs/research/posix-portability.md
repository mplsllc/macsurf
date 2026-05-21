# POSIX Portability Report, NetSurf → Mac OS 9

Comprehensive analysis of every POSIX dependency in the NetSurf codebase and its dependency libraries, with concrete Mac OS 9 replacement strategies, shim sizing, and implementation ordering.

Produced from [netsurf-audit.md](netsurf-audit.md) and [frontend-api-mapping.md](frontend-api-mapping.md).

---

## Section 1, POSIX Dependency Table

### 1.1 File I/O

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `open()` | `content/fs_backing_store.c`, `content/fetchers/file/file.c` | Production | Replace with `FSOpenFork()` from Carbon File Manager. Obtain `FSRef` via `FSPathMakeRef()` (which accepts Unix-style `/` paths under Carbon), then `FSOpenFork()` with `dataForkName`. Return a shim file descriptor that maps to an internal `FSIORefNum`. Flags like `O_RDWR`, `O_CREAT`, `O_TRUNC` map to `fsRdWrPerm`/`fsWrPerm` permissions and `FSCreateFileUnicode()` for creation. | 80 | Yes |
| `close()` | Same as `open()` | Production | `FSCloseFork(refnum)`. The shim maintains a small table mapping integer file descriptors to `FSIORefNum` values; `close()` frees the slot. | 15 | Yes |
| `read()` | `content/fs_backing_store.c`, `content/fetchers/file/file.c` | Production | `FSReadFork(refnum, fsAtMark, 0, requestCount, buffer, &actualCount)`. Semantics match POSIX `read()`, returns bytes read, advances position. The shim translates the fd to an `FSIORefNum` and calls through. | 20 | Yes |
| `write()` | `content/fs_backing_store.c` | Production | `FSWriteFork(refnum, fsAtMark, 0, requestCount, buffer, &actualCount)`. Same fd-to-refnum translation as `read()`. | 20 | Yes |
| `stat()` / `fstat()` | Throughout: `utils/file.c`, `utils/filepath.c`, `content/fs_backing_store.c`, `content/fetchers/file/file.c`, `content/urldb.c`, `content/llcache.c` | Production | `FSPathMakeRef()` to get `FSRef`, then `FSGetCatalogInfo(ref, kFSCatInfoDataSizes | kFSCatInfoContentMod | kFSCatInfoNodeFlags, &info, NULL, NULL, NULL)`. Populate a shim `struct stat` with: `st_size` from `info.dataLogicalSize`, `st_mtime` from `info.contentModDate` (convert Mac epoch 1904 to Unix epoch 1970 by subtracting 2082844800), `st_mode` with `S_IFDIR` from `info.nodeFlags & kFSNodeIsDirectoryMask`. Mac OS 9 has no Unix permissions, always report 0755 for directories, 0644 for files. | 60 | Yes |
| `fstatat()` | `utils/file.c`, `frontends/riscos/filename.c` (not used in MacSurf frontend) | Production (core only) | Construct full path from parent directory path + child name, then call the `stat()` shim. No direct Mac equivalent exists, the shim concatenates paths and delegates. The RISC OS frontend usage won't be compiled for MacSurf. | 25 | Yes |
| `access()` | Multiple: `utils/file.c`, `utils/filepath.c` | Production | `FSPathMakeRef()`, if it returns `noErr`, the file exists. For write checks, attempt `FSOpenFork()` with `fsWrPerm` and immediately close. Mac OS 9 has no Unix permission model, so `R_OK`/`W_OK` checks are existence checks plus lock-status checks via `FSGetCatalogInfo()` with `kFSCatInfoNodeFlags` to test `kFSNodeLockedMask`. | 30 | Yes |
| `realpath()` | `utils/filepath.c`, windows/atari frontends (not MacSurf) | Production (core) | `FSPathMakeRef()` to get `FSRef`, then `FSRefMakePath()` to get the canonical UTF-8 path. This also resolves Finder aliases via `FSResolveAliasFile()` if the `FSRef` points to an alias. The shim combines both calls. | 25 | No, can stub with identity function initially; only affects path canonicalization |
| `mkdir()` | Implied by `utils/file.c` file operations | Production | `FSCreateDirectoryUnicode()` after obtaining parent `FSRef` via `FSPathMakeRef()`. The `mode` parameter is ignored (no Unix permissions on Mac OS 9). Returns the new directory's `FSRef` and `FSSpec`. | 20 | Yes |
| `rmdir()` | `utils/file.c` | Production | `FSPathMakeRef()` + `FSDeleteObject()`. Only succeeds on empty directories, matching POSIX semantics. | 15 | No, only used in cache cleanup; can stub initially |
| `unlink()` | Not directly used but implied by backing store cleanup | Production | `FSPathMakeRef()` + `FSDeleteObject()`. Same call as `rmdir()`, File Manager doesn't distinguish. | 10 | No |
| `opendir()` / `readdir()` / `closedir()` | `utils/utils.c`, `utils/file.c` | Production | `FSOpenIterator(fsRef, kFSIterateFlat, &iterator)` for `opendir()`. `FSGetCatalogInfoBulk(iterator, 1, &count, NULL, kFSCatInfoNodeFlags | kFSCatInfoFinderInfo, &info, &ref, NULL, &name)` for `readdir()`, returns one entry per call, populate a shim `struct dirent` with `d_name` from the `HFSUniStr255` name (convert to UTF-8). `FSCloseIterator()` for `closedir()`. The shim struct wraps `FSIterator` plus state. | 80 | Yes |
| `mmap()` / `munmap()` | `content/fetchers/file/file.c`, libhubbub perf tests | Production (file fetcher) + test | No memory-mapped I/O exists on Mac OS 9. Replace with `FSOpenFork()` + `FSGetForkSize()` + `malloc()` + `FSReadFork()` to read the entire file into a heap buffer. `munmap()` becomes `free()`. The file fetcher uses mmap for `file:///` URLs which are typically small local files (HTML, CSS, images), so heap allocation is acceptable. The libhubbub usage is test-only and not compiled for production. | 40 | Yes |
| `fcntl.h` constants (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`) | `content/fs_backing_store.c`, `content/fetchers/file/file.c` | Production | Define these as integer constants in the shim header. Map to File Manager permission constants internally: `O_RDONLY` → `fsRdPerm`, `O_WRONLY` → `fsWrPerm`, `O_RDWR` → `fsRdWrPerm`. `O_CREAT` and `O_TRUNC` are handled as logic flags in the `open()` shim. | 10 | Yes |
| `sys/types.h` (`off_t`, `size_t`, `ssize_t`, `mode_t`) | Throughout | Production | Define these types in the shim header. `off_t` as `SInt64`, `size_t` as `unsigned long`, `ssize_t` as `long`, `mode_t` as `unsigned short`. CodeWarrior 8 provides `size_t` via `<stddef.h>` but the others need explicit typedefs. | 10 | Yes |

### 1.2 Networking

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `socket()` | `desktop/gui_factory.c` line 549 | Production | This single call is used in the `gui_fetch_table.socket_open` default implementation. MacSurf will provide its own `gui_fetch_table` with `socket_open` implemented via `OTOpenEndpoint(OTCreateConfiguration("tcp"), 0, NULL, &err)`. The frontend callback replaces this, so no shim is needed, just implement the callback. | 0 (callback) | No, frontend callback replaces it |
| `select()` | `frontends/monkey/main.c` line 325 | Test/debug frontend only | Not used in any production frontend. The monkey frontend is a testing tool. MacSurf will not compile the monkey frontend. No replacement needed. | 0 | No |
| `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` | `utils/inet.h` | Production (header types) | Provide a `mac_inet.h` that defines `struct sockaddr_in`, `in_addr_t`, `AF_INET`, `SOCK_STREAM`, `htons()`/`ntohs()`/`htonl()`/`ntohl()` (no-ops on big-endian PPC), and `inet_aton()`/`inet_ntoa()`. Open Transport provides these via `<OpenTransportProviders.h>` which defines `InetAddress`, `OTInitInetAddress()`, etc. The shim maps POSIX names to OT names. | 50 | Yes |
| `inet_aton()` / `inet_pton()` | `utils/inet.h` | Production | `OTInetStringToHost()` converts dotted-quad string to `InetHost` (32-bit address). Wrap in a function matching `inet_aton()` signature. `inet_pton()` for IPv4 is the same; IPv6 is not applicable (Mac OS 9 has no IPv6 support, disable via `HAVE_INETPTON=0`). | 20 | No, can disable via config.h macro |

### 1.3 Signals

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `signal(SIGPIPE, SIG_IGN)` | `desktop/netsurf.c` line 132 | Production | Mac OS 9 has no Unix signals. Open Transport reports errors inline via `kOTLookErr` and similar result codes rather than raising `SIGPIPE`. Define `HAVE_SIGPIPE=0` in `utils/config.h` under `__MACOS9__`, which causes the `#if defined(HAVE_SIGPIPE)` guard to skip this call. Matches the RISC OS and Windows approach. | 0 | No, config.h macro |
| `signal(SIGSEGV, ...)` etc. | `frontends/monkey/`, `frontends/riscos/` crash handlers | Not applicable | These are in other frontends, not compiled for MacSurf. No replacement needed. If crash handling is desired later, use `ExceptionInformation` and Macsbug, but this is entirely optional. | 0 | No |

### 1.4 Time

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `gettimeofday()` | `utils/log.c`, windows/monkey/framebuffer/riscos frontends | Production (logging) | `Microseconds(&usecs)` returns a 64-bit `UnsignedWide` value of microseconds since boot. For wall-clock time, use `GetDateTime(&secs)` which returns seconds since 1904-01-01, then subtract 2082844800 for Unix epoch. Populate `struct timeval` with `tv_sec` and `tv_usec` (from `Microseconds()` modulo 1000000). The shim provides both monotonic and wall-clock variants. | 25 | Yes |
| `time()` | `content/fs_backing_store.c`, `content/urldb.c`, `content/llcache.c` | Production | `GetDateTime(&secs)` returns seconds since 1904-01-01. Subtract 2082844800 to convert to Unix epoch (seconds since 1970-01-01). Return as `time_t`. | 10 | Yes |
| `localtime()` / `gmtime()` | `desktop/global_history.c`, `utils/time.c` | Production | `SecondsToDate(macSeconds, &dateTimeRec)` populates a `DateTimeRec` with year/month/day/hour/minute/second/dayOfWeek. Map fields to `struct tm`: `tm_year = dateTimeRec.year - 1900`, `tm_mon = dateTimeRec.month - 1`, `tm_mday = dateTimeRec.day`, etc. For `gmtime()`, Mac OS 9 has no native UTC conversion, use `ReadLocation(&machineLocation)` to get GMT offset and adjust before conversion. | 40 | Yes |
| `mktime()` | `utils/time.c` | Production | `DateToSeconds(&dateTimeRec, &secs)`, populate a `DateTimeRec` from `struct tm` fields and convert to Mac epoch seconds, then subtract 2082844800 for Unix epoch. | 15 | Yes |
| `strftime()` | `utils/time.c`, `desktop/global_history.c` | Production | Manual formatting from `struct tm` fields. NetSurf uses limited format strings (`%Y-%m-%d`, `%H:%M`, etc.), implement only the format specifiers actually used. Alternatively, `IntlDateToString()` for locale-aware formatting, but the manual approach is simpler and more predictable. | 50 | Yes |
| `strptime()` | `utils/time.c` | Production | Limited usage, already disabled on RISC OS via `HAVE_STRPTIME=0`. Define `HAVE_STRPTIME=0` in the `__MACOS9__` config.h block. NetSurf falls back to manual parsing when strptime is unavailable. | 0 | No, config.h macro |
| `clock_gettime(CLOCK_MONOTONIC)` | duktape only | Not applicable | Duktape is the JavaScript engine. MacSurf disables JavaScript entirely (`WITHOUT_DUKTAPE`), so this code is never compiled. No replacement needed. | 0 | No |

### 1.5 Character Encoding

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `iconv_open()` | `utils/utf8.c`, `libparserutils/src/input/filter.c` | Production, critical | `TECCreateConverter(&tecObject, sourceEncoding, destinationEncoding)` from the TextEncoding Converter API (`<TextEncodingConverter.h>`, available since Mac OS 8.5). Map IANA charset names (e.g., "UTF-8", "ISO-8859-1") to `TextEncoding` constants via `TECGetTextEncodingFromInternetName()`, which handles most standard IANA names natively. Cache `TECObjectRef` instances per encoding pair to avoid repeated creation overhead. Return a shim `iconv_t` descriptor that wraps the `TECObjectRef`. | 60 | Yes |
| `iconv()` | Same as above | Production, critical | `TECConvertText(tecObject, inputBuffer, inputLength, &inputRead, outputBuffer, outputLength, &outputWritten)`. Semantics closely match iconv: processes as much input as fits in the output buffer, returns partial conversion status. Map TEC error codes to iconv-compatible errors: `kTECPartialCharErr` → set `errno = EINVAL` (incomplete multibyte), `kTECOutputBufferFullStatus` → set `errno = E2BIG`, `kTECUnmappableElementErr` → set `errno = EILSEQ`. Update input/output pointer and length parameters to match iconv's in-out semantics. | 50 | Yes |
| `iconv_close()` | Same as above | Production, critical | `TECDisposeConverter(tecObject)`. Free the shim descriptor. | 10 | Yes |

### 1.6 Directory and Path Utilities

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `dirent.h` types (`DIR`, `struct dirent`) | `utils/utils.c`, `utils/file.c` | Production | Define `struct dirent` with `d_name[256]` (HFS+ supports 255 Unicode chars) and `d_type` (set `DT_DIR` or `DT_REG` from `kFSNodeIsDirectoryMask`). `DIR` wraps `FSIterator` plus parent `FSRef`. Provided by the `mac_dirent.c` shim. | Included in opendir/readdir above | Yes |

### 1.7 Dynamic Loading

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `dlsym()` / `dlfcn.h` | `test/malloc_fig.c` | Test-only | Not needed. This is a test utility that interposes malloc for memory profiling. Not compiled for production builds. | 0 | No |

### 1.8 Miscellaneous

| POSIX Call | File(s) Where Used | Production or Test-Only? | Mac OS 9 Replacement Strategy | Est. Lines of Shim | Blocking? |
|---|---|---|---|---|---|
| `unistd.h` general | `utils/file.c`, `utils/filepath.c`, `content/fs_backing_store.c`, `content/fetchers/file/file.c` | Production | Not a function but a header. The shim header `mac_posix.h` replaces it by providing the function prototypes for `read()`, `write()`, `close()`, `access()`, `unlink()`, `rmdir()`, etc. that are implemented in the shim .c files. | 15 (header) | Yes |
| `sys/select.h` | `utils/inet.h` | Production (header) | Guarded by `HAVE_SYS_SELECT` in `utils/config.h`. Define `HAVE_SYS_SELECT=0` under `__MACOS9__`. Open Transport is event-driven via notifiers, not select-based. | 0 | No, config.h macro |
| `uname()` / `sys/utsname.h` | Used in some frontends for user-agent string | Production (optional) | Define `HAVE_UTSNAME=0` in config.h. Provide a hardcoded user-agent string component ("MacOS9; PPC") in the frontend instead of querying at runtime. The RISC OS and Windows frontends already handle this via static strings. | 0 | No, config.h macro |

---

## Section 2, Shim Implementation Order

Ordered by compilation dependency: each shim must exist before the files that depend on it can compile. Libraries (libparserutils, libcss, libhubbub, libdom, libwapcaplet) compile first, then NetSurf core, then the MacSurf frontend.

### Phase 0, Library Dependencies

#### 1. `mac_iconv.c` / `mac_iconv.h`

**Covers:** `iconv_open()`, `iconv()`, `iconv_close()`

**Why first:** libparserutils (`src/input/filter.c`) requires iconv at compile time. libparserutils is the lowest dependency, libcss, libhubbub, and libdom all depend on it. Nothing in the library stack compiles without a working iconv replacement.

**Depends on:** TextEncoding Converter API (`<TextEncodingConverter.h>`)

**NetSurf files that depend on this:**
- `libparserutils/src/input/filter.c`, charset conversion during HTML/CSS parsing
- `utils/utf8.c`, UTF-8 ↔ local encoding conversion throughout the core

**Estimated size:** 120 lines

---

### Phase 1, Core POSIX Shims

These shims are needed to compile NetSurf core (`content/`, `utils/`, `desktop/`).

#### 2. `mac_types.h`

**Covers:** `sys/types.h` types (`off_t`, `ssize_t`, `mode_t`), `fcntl.h` constants (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`), `unistd.h` prototypes

**Why second:** Every other shim and most NetSurf source files include `sys/types.h` or `unistd.h`. This header provides the type definitions and function declarations that the other shim .c files implement.

**Depends on:** CodeWarrior standard headers (`<stddef.h>`, `<MacTypes.h>`)

**NetSurf files that depend on this:**
- `utils/file.c`, `utils/filepath.c`
- `content/fs_backing_store.c`, `content/fetchers/file/file.c`
- `content/urldb.c`, `content/llcache.c`
- Virtually all source files via transitive includes

**Estimated size:** 60 lines (header only)

#### 3. `mac_file_io.c` / `mac_file_io.h`

**Covers:** `open()`, `close()`, `read()`, `write()`, `unlink()`

**Why third:** The backing store (`content/fs_backing_store.c`) and file fetcher (`content/fetchers/file/file.c`) are core components that must compile. They use low-level POSIX file I/O throughout.

**Depends on:** `mac_types.h`, Carbon File Manager (`<Files.h>`)

**NetSurf files that depend on this:**
- `content/fs_backing_store.c`, disk-backed content cache
- `content/fetchers/file/file.c`, `file:///` URL handler

**Estimated size:** 145 lines

#### 4. `mac_stat.c` / `mac_stat.h`

**Covers:** `stat()`, `fstat()`, `fstatat()`, `access()`, `struct stat`, `S_IFDIR`/`S_IFREG` constants

**Why fourth:** Used throughout NetSurf core for file existence checks, size queries, and modification time comparisons. The backing store and URL database both call `stat()` frequently.

**Depends on:** `mac_types.h`, Carbon File Manager (`<Files.h>`)

**NetSurf files that depend on this:**
- `utils/file.c`, file utility functions
- `utils/filepath.c`, path resolution
- `content/fs_backing_store.c`, cache management
- `content/fetchers/file/file.c`, file fetcher
- `content/urldb.c`, URL database
- `content/llcache.c`, low-level cache

**Estimated size:** 115 lines

#### 5. `mac_dirent.c` / `mac_dirent.h`

**Covers:** `opendir()`, `readdir()`, `closedir()`, `DIR`, `struct dirent`

**Why fifth:** Used by `utils/file.c` and `utils/utils.c` for directory traversal, needed for cache directory enumeration and resource file discovery.

**Depends on:** `mac_types.h`, Carbon File Manager (`<Files.h>`, `FSIterator` API)

**NetSurf files that depend on this:**
- `utils/utils.c`, general utility directory operations
- `utils/file.c`, file/directory operations

**Estimated size:** 80 lines

#### 6. `mac_time.c` / `mac_time.h`

**Covers:** `gettimeofday()`, `time()`, `localtime()`, `gmtime()`, `mktime()`, `strftime()`

**Why sixth:** Used by the backing store for cache expiry timestamps, URL database for visit times, logging for timestamps, and global history for date display. Not needed for initial compilation of most files but required for linking.

**Depends on:** `mac_types.h`, `<DateTimeUtils.h>`, `<Timer.h>` (`Microseconds()`)

**NetSurf files that depend on this:**
- `utils/log.c`, log timestamps
- `utils/time.c`, time parsing/formatting utilities
- `content/fs_backing_store.c`, cache expiry
- `content/urldb.c`, URL visit timestamps
- `content/llcache.c`, content freshness
- `desktop/global_history.c`, history date display

**Estimated size:** 140 lines

#### 7. `mac_mmap.c` / `mac_mmap.h`

**Covers:** `mmap()`, `munmap()`

**Why seventh:** Only used in the file fetcher (`content/fetchers/file/file.c`). The file fetcher can be compiled with the other shims in place and this one added for linking.

**Depends on:** `mac_file_io.h`, `mac_types.h`, Carbon File Manager

**NetSurf files that depend on this:**
- `content/fetchers/file/file.c`, `file:///` URL loading

**Estimated size:** 40 lines

### Phase 2, Networking and Frontend Shims

#### 8. `mac_inet.h`

**Covers:** `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` types and constants, `inet_aton()`

**Why eighth:** Required to compile `utils/inet.h` which is included by several core files. Most networking is handled by the frontend's `gui_fetch_table` callbacks and Open Transport, but the type definitions must exist.

**Depends on:** Open Transport (`<OpenTransport.h>`, `<OpenTransportProviders.h>`)

**NetSurf files that depend on this:**
- `utils/inet.h`, internet address utilities

**Estimated size:** 50 lines (mostly typedefs and macros)

#### 9. `mac_path.c` / `mac_path.h`

**Covers:** `realpath()`, `mkdir()`, `rmdir()`

**Why ninth:** Used for path canonicalization and directory creation. Can be deferred slightly because `mkdir()` is only needed when creating cache directories (which happens at runtime, not compile time), and `realpath()` can be stubbed with an identity function initially.

**Depends on:** `mac_types.h`, Carbon File Manager

**NetSurf files that depend on this:**
- `utils/filepath.c`, path resolution
- `utils/file.c`, directory creation/removal

**Estimated size:** 60 lines

---

### Shim Dependency Graph

```
mac_iconv.c          (standalone — compiles against TEC headers only)
    ↓
mac_types.h          (standalone — type definitions only)
    ↓
mac_file_io.c        (depends on mac_types.h)
mac_stat.c           (depends on mac_types.h)
mac_dirent.c         (depends on mac_types.h)
mac_time.c           (depends on mac_types.h)
mac_mmap.c           (depends on mac_types.h, mac_file_io.h)
mac_inet.h           (standalone — OT type mappings)
mac_path.c           (depends on mac_types.h)
```

---

## Section 3, `utils/config.h` Additions

New `#elif defined(__MACOS9__)` block, modeled on the existing `__riscos__` and `_WIN32` blocks. Each macro listed with its value and rationale.

### Feature Availability Macros

| Macro | Define or Undef | Value | Rationale |
|---|---|---|---|
| `HAVE_STRPTIME` | Undef | `0` | Mac OS 9 has no `strptime()`. Match RISC OS behavior, NetSurf falls back to manual HTTP date parsing in `utils/time.c` |
| `HAVE_STRFTIME` | Define | `1` | Provided by `mac_time.c` shim. NetSurf needs this for history date formatting. Unlike RISC OS which also lacks it, our shim implements the required subset |
| `HAVE_STRCHRNUL` | Undef | `0` | GNU extension, not available. NetSurf provides a fallback implementation when undefined |
| `HAVE_SYS_SELECT` | Undef | `0` | No `sys/select.h` on Mac OS 9. Open Transport uses notifier callbacks instead of select(). Guards the include in `utils/inet.h` |
| `HAVE_POSIX_INET_HEADERS` | Undef | `0` | No `sys/socket.h`, `netinet/in.h`, `arpa/inet.h` in standard form. `mac_inet.h` provides equivalent types under different names. This macro guards includes in `utils/inet.h` |
| `HAVE_INETATON` | Undef | `0` | No native `inet_aton()`. `mac_inet.h` provides a wrapper over `OTInetStringToHost()`, but defining this as 0 lets NetSurf use its internal fallback which is simpler |
| `HAVE_INETPTON` | Undef | `0` | No `inet_pton()`. Mac OS 9 has no IPv6 support. NetSurf's IPv4 paths do not require this |
| `HAVE_UTSNAME` | Undef | `0` | No `sys/utsname.h`. User-agent string component will be hardcoded in the frontend |
| `HAVE_REALPATH` | Undef | `0` | No native `realpath()`. The `mac_path.c` shim provides one via `FSPathMakeRef()` + `FSRefMakePath()`, but defining this as 0 initially lets NetSurf use its internal fallback path handling |
| `HAVE_MKDIR` | Define | `1` | Provided by `mac_path.c` shim via `FSCreateDirectoryUnicode()`. NetSurf needs this for cache directory creation |
| `HAVE_SIGPIPE` | Undef | `0` | Mac OS 9 has no Unix signals. Open Transport reports errors inline. Matches RISC OS and Windows |
| `HAVE_STDOUT` | Define | `1` | Can write to stdout via CodeWarrior's SIOUX console or redirect to file. Useful for debug builds. Set to 0 for release builds if no console output is desired |
| `HAVE_MMAP` | Undef | `0` | No memory-mapped I/O on Mac OS 9. The file fetcher falls back to `read()`-based loading when this is 0. Even though `mac_mmap.c` provides a heap-based shim, setting this to 0 avoids the mmap code path in favor of the simpler `read()` path if NetSurf has one |
| `HAVE_DIRFD` | Undef | `0` | No `dirfd()` on Mac OS 9, `DIR` wraps `FSIterator`, not a file descriptor. Matches RISC OS, Windows, AmigaOS |
| `HAVE_UNLINKAT` | Undef | `0` | No `unlinkat()`, POSIX.1-2008 function not available. `unlink()` shim via `FSDeleteObject()` covers the use case. Matches RISC OS, Windows, AmigaOS |
| `HAVE_FSTATAT` | Undef | `0` | No native `fstatat()`. The `mac_stat.c` shim provides a path-concatenation wrapper, but defining this as 0 lets NetSurf use its internal fallback that calls `stat()` on a constructed path. Matches RISC OS, Windows, AmigaOS |
| `HAVE_SCANDIR` | Undef | `0` | No `scandir()`. Not widely used in NetSurf core, `opendir()`/`readdir()` cover all directory traversal needs |
| `HAVE_REGEX` | Undef | `0` | No POSIX regex. Not required for core browser functionality, only used in content filtering which can be disabled |
| `HAVE_EXECINFO` | Undef | `0` | No `<execinfo.h>` or `backtrace()`. Only used for crash stack traces on Linux/glibc. Not applicable |

### Build Configuration Macros

| Macro | Define or Undef | Value | Rationale |
|---|---|---|---|
| `WITHOUT_DUKTAPE` | Define | `1` | JavaScript is explicitly excluded from MacSurf per CLAUDE.md. This disables all duktape compilation, eliminating `clock_gettime()` and other JS-engine POSIX dependencies |
| `WITH_CURL` | Undef | not defined | MacSurf does not use cURL, all fetching goes through Open Transport to the MacSurf proxy via plain HTTP. Eliminates cURL as a build dependency |
| `WITH_OPENSSL` | Undef | not defined | No TLS in the browser, the proxy handles all HTTPS. Eliminates OpenSSL as a build dependency |
| `WITH_PDF_EXPORT` | Undef | not defined | No PDF export planned for Mac OS 9 |

### Proposed config.h Block

The actual `#elif` should be structured as (for reference, not code to write yet):

```
#elif defined(__MACOS9__)
/* Mac OS 9 / Carbon — cooperative multitasking, no POSIX layer */
#define HAVE_STRFTIME      1
#define HAVE_MKDIR         1
#define HAVE_STDOUT        1

/* Unavailable POSIX features */
#undef HAVE_STRPTIME
#undef HAVE_STRCHRNUL
#undef HAVE_SYS_SELECT
#undef HAVE_POSIX_INET_HEADERS
#undef HAVE_INETATON
#undef HAVE_INETPTON
#undef HAVE_UTSNAME
#undef HAVE_REALPATH
#undef HAVE_SIGPIPE
#undef HAVE_MMAP
#undef HAVE_DIRFD
#undef HAVE_UNLINKAT
#undef HAVE_FSTATAT
#undef HAVE_SCANDIR
#undef HAVE_REGEX
#undef HAVE_EXECINFO
```

This block is closest to the RISC OS block (both are cooperative-multitasking non-POSIX systems) with additional networking macros disabled (matching Windows) because Mac OS 9 uses Open Transport instead of BSD sockets.
