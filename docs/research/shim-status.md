# MacSurf POSIX Shim Status

Shim layer in `browser/netsurf/frontends/macos9/shims/`. Follows the implementation order from `posix-portability.md` Section 2.

---

## Phase 0, Library Dependencies

### mac_iconv.h / mac_iconv.c, Character encoding shim

Wraps the Mac OS 9 Text Encoding Converter API behind standard iconv signatures.

| Function | Mac OS 9 Implementation | Linux Stub |
|---|---|---|
| `iconv_open(tocode, fromcode)` | `TECGetTextEncodingFromInternetName()` for both names, then `TECCreateConverter()`. Returns allocated descriptor wrapping `TECObjectRef`. Cached per encoding pair. | Returns `(iconv_t)-1` |
| `iconv(cd, inbuf, inleft, outbuf, outleft)` | `TECConvertText()`. Error mapping: `kTECPartialCharErr` → `EINVAL`, `kTECOutputBufferFullStatus` → `E2BIG`, `kTECUnmappableElementErr` → `EILSEQ`. Updates pointers in-place. | Returns `(size_t)-1` |
| `iconv_close(cd)` | `TECDisposeConverter()`. Decrements refcount; frees and removes from cache when zero. | Returns `0` |

Converter cache: up to 16 `TECObjectRef` instances, keyed by tocode/fromcode pair. Refcounted, same pair opened multiple times shares the converter.

All TEC calls gated with `#ifdef __MACOS9__`.

---

## Phase 1, Core POSIX Shims

### mac_types.h, Type definitions and forward declarations

Provides POSIX type replacements and constants for Mac OS 9, plus forward declarations for all shim implementation files.

| Category | Contents |
|---|---|
| `sys/types.h` | `off_t` (SInt64), `ssize_t` (long), `mode_t` (unsigned short) |
| `fcntl.h` | `O_RDONLY` (0), `O_WRONLY` (1), `O_RDWR` (2), `O_CREAT` (0x200), `O_TRUNC` (0x400) |
| `sys/stat.h` | `S_IFDIR`, `S_IFREG`, `S_ISDIR()`, `S_ISREG()`, minimal `struct stat` |
| `errno.h` | `EINVAL` (22), `E2BIG` (7), `EILSEQ` (84), guarded with `#ifndef` |
| Forward decls | `mac_file_io.c`: `mac_open`, `mac_close`, `mac_read`, `mac_write`, `mac_unlink` |
| | `mac_stat.c`: `mac_stat`, `mac_fstat`, `mac_access` |
| | `mac_dirent.c`: `mac_DIR`, `mac_dirent`, `mac_opendir`, `mac_readdir`, `mac_closedir` |
| | `mac_time.c`: `mac_gettimeofday`, `mac_time`, `mac_localtime`, `mac_gmtime`, `mac_mktime`, `mac_strftime` |

On Linux, includes `<sys/types.h>` and `<sys/stat.h>` directly; Mac-only types gated with `#ifdef __MACOS9__`.

---

## Compilation Results

```
=== mac_iconv.h ===  ✓ clean (0 errors, 0 warnings)
=== mac_iconv.c ===  ✓ clean (0 errors, 0 warnings)
=== mac_types.h ===  ✓ clean (0 errors, 0 warnings)
```

Verified with `gcc -fsyntax-only -std=c99` from the NetSurf root.

---

## Phase 1, Core POSIX Shims (File I/O, Stat, Dirent, Time)

### mac_file_io.h / mac_file_io.c, File I/O shim

Internal fd table: fixed array of 64 slots mapping `int fd` → `FSIORefNum` + `FSRef`.

| Function | Mac OS 9 Implementation | Linux Stub |
|---|---|---|
| `mac_open(path, flags, ...)` | `FSPathMakeRef()` + `FSOpenFork()`. `O_CREAT` → `FSCreateFileUnicode()` if file missing. `O_TRUNC` → `FSSetForkSize(refnum, fsFromStart, 0)`. Returns fd slot index. | Returns `-1` |
| `mac_close(fd)` | `FSCloseFork(refnum)`, free slot | Returns `-1` |
| `mac_read(fd, buf, count)` | `FSReadFork(refnum, fsAtMark, 0, count, buf, &actual)`, returns actual. Handles `eofErr`. | Returns `-1` |
| `mac_write(fd, buf, count)` | `FSWriteFork(refnum, fsAtMark, 0, count, buf, &actual)`, returns actual | Returns `-1` |
| `mac_unlink(path)` | `FSPathMakeRef()` + `FSDeleteObject()` | Returns `-1` |
| `mac_fd_get_refnum(fd, out_refnum, out_fsref)` | Copies `FSIORefNum` and `FSRef` from fd table slot. Used by `mac_fstat()`. | Returns `-1` |

Permission mapping: `O_RDWR` → `fsRdWrPerm`, `O_WRONLY` → `fsWrPerm`, default → `fsRdPerm`.

### mac_stat.h / mac_stat.c, stat/fstat/access shim

| Function | Mac OS 9 Implementation | Linux Stub |
|---|---|---|
| `mac_stat(path, buf)` | `FSPathMakeRef()` + `FSGetCatalogInfo(kFSCatInfoDataSizes \| kFSCatInfoContentMod \| kFSCatInfoNodeFlags)`. `st_size` from `dataLogicalSize`, `st_mtime` from `contentModDate` (subtract 2082844800 for Unix epoch), `st_mode` with `S_IFDIR`/0755 or `S_IFREG`/0644. | Returns `-1` |
| `mac_fstat(fd, buf)` | Looks up `FSIORefNum` + `FSRef` via `mac_fd_get_refnum()`. `FSGetForkSize()` for `st_size`, `FSGetCatalogInfo()` for mtime and node flags. | Returns `-1` |
| `mac_access(path, mode)` | `FSPathMakeRef()` existence check. Always returns 0 if exists, no Unix permissions on OS 9. | Returns `-1` |

### mac_dirent.h / mac_dirent.c, Directory iteration shim

`mac_DIR` wraps `FSIterator` + parent `FSRef` + one-entry `struct mac_dirent` buffer.

| Function | Mac OS 9 Implementation | Linux Stub |
|---|---|---|
| `mac_opendir(path)` | `FSPathMakeRef()` + `FSOpenIterator(ref, kFSIterateFlat, &iterator)` | Returns `NULL` |
| `mac_readdir(dir)` | `FSGetCatalogInfoBulk()` for one entry. Converts `HFSUniStr255` name to UTF-8 via TEC (`TECCreateConverter` Unicode→UTF-8). Sets `d_type` from `kFSNodeIsDirectoryMask`. | Returns `NULL` |
| `mac_closedir(dir)` | `FSCloseIterator()`, `free(dir)` | Returns `0` |

`struct mac_dirent` has `d_name[256]` and `d_type` (`DT_DIR=4`, `DT_REG=8`).

### mac_time.h / mac_time.c, Time function shim

| Function | Mac OS 9 Implementation | Linux Stub |
|---|---|---|
| `mac_gettimeofday(tv, tz)` | `GetDateTime()` for `tv_sec` (subtract 2082844800), `Microseconds()` for `tv_usec` (mod 1000000) | Returns `0`, zeroed tv |
| `mac_time(t)` | `GetDateTime()` minus 2082844800 | Returns `0` |
| `mac_localtime(timep)` | `SecondsToDate()` on Mac-epoch seconds → populate `struct mac_tm` | Returns zeroed `mac_tm` |
| `mac_gmtime(timep)` | `ReadLocation()` for GMT offset (24-bit signed `gmtDelta`), subtract from Mac seconds, then `SecondsToDate()` | Returns zeroed `mac_tm` |
| `mac_mktime(tm)` | Populate `DateTimeRec` from `struct mac_tm`, `DateToSeconds()`, subtract 2082844800 | Returns `0` |
| `mac_strftime(s, max, fmt, tm)` | Manual format: `%Y %m %d %H %M %S %A %B %a %b %% `. Pure C, no Toolbox calls. Shared between Mac and Linux builds. | Same implementation |

Mac epoch offset: 2082844800 seconds (1904-01-01 → 1970-01-01).

### mac_types.h, Updated

Added to existing file:
- `time_t` typedef (long) under `__MACOS9__`
- `d_type` field (unsigned char) in `struct mac_dirent`
- `DT_DIR` (4), `DT_REG` (8) constants
- `F_OK`, `R_OK`, `W_OK`, `X_OK` access mode constants
- `mac_fd_get_refnum()` forward declaration

---

## Compilation Results

```
=== mac_iconv.h ===    ✓ clean (0 errors, 0 warnings)
=== mac_iconv.c ===    ✓ clean (0 errors, 0 warnings)
=== mac_types.h ===    ✓ clean (0 errors, 0 warnings)
=== mac_file_io.h ===  ✓ clean (0 errors, 0 warnings)
=== mac_file_io.c ===  ✓ clean (0 errors, 0 warnings)
=== mac_stat.h ===     ✓ clean (0 errors, 0 warnings)
=== mac_stat.c ===     ✓ clean (0 errors, 0 warnings)
=== mac_dirent.h ===   ✓ clean (0 errors, 0 warnings)
=== mac_dirent.c ===   ✓ clean (0 errors, 0 warnings)
=== mac_time.h ===     ✓ clean (0 errors, 0 warnings)
=== mac_time.c ===     ✓ clean (0 errors, 0 warnings)
```

Verified with `gcc -fsyntax-only -std=c99` from the NetSurf root.

---

## bool / true / false ordering fix

**Problem:** `MacTypes.h` (Mac Toolbox) defines `enum { false = 0, true = 1 };`. If `<stdbool.h>` is included first, `true` and `false` become preprocessor macros. The preprocessor then expands the enum member names to `enum { 0, 1 };`, illegal C that CodeWarrior rejects at MacTypes.h line 301-303.

**Fix (2 files changed):**

1. **`shims/mac_types.h`**, Added a `bool` / `true` / `false` block at the bottom of the file, guarded by `#ifndef bool`. Uses `typedef unsigned char bool;` plus `#define true 1` / `#define false 0`. Positioned after all other definitions so that on Mac OS 9 it comes after Mac Toolbox headers have been included.

2. **`macos9.h`**, Removed top-level `#include <stdbool.h>`. Under `__MACOS9__`, Mac Toolbox headers (`<MacWindows.h>`, `<Controls.h>`) are included first, then `shims/mac_types.h` provides `bool`. Under the Linux `#else` path, `<stdbool.h>` is retained.

**Verification:** `gcc -fsyntax-only -std=c99` passes clean on both `mac_types.h` and `macos9.h` (Linux build). The `#ifndef bool` guard ensures the block is skipped when `<stdbool.h>` is already included.

---

## CW8 C89 Compatibility Sweep

Audit and fix pass across all frontend .c files and shim headers for CodeWarrior 8 C89 compliance.

### bool / MacTypes include ordering (11 files)

**Problem:** `#include <stdbool.h>` appeared before `#include "macos9/macos9.h"` in every frontend .c file. On CW8, `stdbool.h` would `#define true 1` / `#define false 0` before `MacTypes.h` (pulled in via `<MacWindows.h>` inside `macos9.h`) could declare its `enum { false, true }`, producing illegal C.

**Fix:** Wrapped `#include <stdbool.h>` with `#ifndef __MACOS9__` / `#endif` in all 11 files: `main.c`, `window.c`, `schedule.c`, `misc.c`, `plotters.c`, `font.c`, `clipboard.c`, `macos9_bitmap.c`, `macos9_download.c`, `macos9_fetch.c`, `macos9_utf8.c`. On the Mac build path, `macos9.h` → `mac_types.h` provides bool/true/false after Mac headers.

### FSIORefNum undefined (mac_types.h)

**Problem:** `FSIORefNum` (used in `mac_file_io.c` struct) is provided by `<Files.h>`, but `mac_types.h` may be parsed before `<Files.h>` is included (e.g. via `mac_file_io.h`).

**Fix:** Added `typedef short FSIORefNum;` inside `#ifdef __MACOS9__`, guarded by `#ifndef __FILES__` to avoid redefinition when Carbon's `Files.h` is also included.

### TEC header in mac_dirent.c

**Problem:** `mac_dirent.c` included `<UnicodeConverter.h>` instead of `<TextEncodingConverter.h>`. The TEC types (`TECObjectRef`, `TECCreateConverter`, etc.) used in `uni_to_utf8()` are declared in `TextEncodingConverter.h`.

**Fix:** Changed include to `<TextEncodingConverter.h>`.

### inline keyword (mac_inet.h)

**Problem:** `mac_inet.h` used `static inline` on 5 functions (`htons`, `ntohs`, `htonl`, `ntohl`, `socket`). CW8 C89 mode rejects `inline`.

**Fix:** Changed all `static inline` to `static`. These are in a header, so `static` provides the same effect (each TU gets its own copy; linker deduplicates).

### C99 designated initializers (10 tables across 9 files)

**Problem:** Every `gui_*_table` and `plotter_table` initialization used C99 designated initializers (`.field = value`). Not supported in C89.

**Fix:** Converted all tables to positional (C89) initializers with field-order comments referencing the header. Tables converted:

| File | Table |
|---|---|
| `main.c` | `struct netsurf_table` (converted to memset + field assignment) |
| `window.c` | `gui_window_table` (20 fields) |
| `plotters.c` | `plotter_table` (13 fields) |
| `misc.c` | `gui_misc_table` (6 fields) |
| `macos9_fetch.c` | `gui_fetch_table` (7 fields, 4 NULL) |
| `clipboard.c` | `gui_clipboard_table` (2 fields) |
| `font.c` | `gui_layout_table` (3 fields) |
| `macos9_bitmap.c` | `gui_bitmap_table` (10 fields) |
| `macos9_download.c` | `gui_download_table` (4 fields) |
| `macos9_utf8.c` | `gui_utf8_table` (2 fields) |

### strndup unavailable (macos9_utf8.c)

**Problem:** `strndup()` is POSIX.1-2008, not available in CW8's C89 runtime. Used in `macos9_utf8_to_local()` and `macos9_local_to_utf8()`.

**Fix:** Added `macos9_strndup()` static implementation in `macos9_utf8.c`, `#define strndup macos9_strndup` under `__MACOS9__`. Linux build continues to use libc `strndup`.

### No issues found

- **`macos9/macos9.h` path:** All files use `#include "macos9/macos9.h"` and the file exists at the expected location. No issues.
- **`for(int i=...)` C99 declarations:** None found in frontend code.
- **Compound literals `(struct foo){...}`:** None found in frontend code.
- **`strndup` in shim files:** Not used in any shim file, only in `macos9_utf8.c`.

---

### Compilation Results (post-fix)

```
=== All frontend .c files ===  ✓ clean (0 errors, 0 warnings from our code)
=== All shim .c files ===      ✓ clean (0 errors, 0 warnings)
=== mac_types.h ===            ✓ clean
=== mac_inet.h ===             ✓ clean
```

Verified with `gcc -fsyntax-only -std=c89 -pedantic` from the NetSurf root (Linux build path).

---

## What's Not Here Yet

- `mac_mmap.c`, NewPtr-based mmap shim (Phase 1)
- `mac_inet.h`, Open Transport type mappings (Phase 2)
- `mac_path.c`, FSSpec-based path operations (Phase 2)
