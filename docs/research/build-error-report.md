# MacSurf Build Error Report (Build 13)

**Date:** 2026-04-09
**Source:** `errors.txt` after Build 13 fixes
**Total errors:** 3,900 (REGRESSION from ~269, **+3,631**)

---

## Progress Across All Builds

| Build | Errors | Delta | Key Fix |
|-------|-------:|------:|---------|
| 1 | 1,374 |, | Baseline |
| ... | ... | ... | ... |
| 6 | 269 |, | Previous low point |
| 13 | **3,900** | **+3,631** | **REGRESSION, prefix file broke the entire build** |

---

## Root Cause: Two Catastrophic Issues in macsurf_prefix.h

The prefix file (lines 14-17) added in this build broke everything:

```c
#include <stddef.h>   /* line 14 — OK, MSL provides this */
#include <string.h>   /* line 15 — OK, MSL provides this */
#include <time.h>     /* line 16 — WRONG: finds NetSurf's utils/time.h, NOT MSL's */
#include <stat.h>     /* line 17 — FAILS: stat.h shim was deleted, MSL doesn't have <stat.h> */
```

### Issue 1: `#include <time.h>` Finds the Wrong Header (3,861 errors)

CW8 searches system paths for `<time.h>`. The access path order is:
1. CIncludes, MacHeaders (CW8 system), MSL's `<time.h>` lives here
2. `frontends/macos9/` stubs (our system paths)
3. MSL_C paths

BUT `utils/` is on the **user** access path, and CW8's `AlwaysSearchUserPaths` may still be `true` (or `<time.h>` resolves to NetSurf's `utils/time.h` through another mechanism). The result is that `#include <time.h>` in the prefix finds **NetSurf's `utils/time.h`** which declares functions using `nserror`, `time_t`, and other types that don't exist yet at this point in the prefix.

This causes `nsc_sntimet`, `rfc1123_date`, etc. to fail with `illegal function definition`, and since the prefix is injected before every compilation unit, **this breaks every single file** (39 compilation units × ~100 cascade errors each = ~3,861 errors).

### Issue 2: `#include <stat.h>` File Not Found (39 errors)

We deleted `shims/stat.h` and assumed MSL provides `<stat.h>`. But MSL's stat functions are either:
- In a different header path (e.g., `<sys/stat.h>`)
- Not available as a bare `<stat.h>` on this CW8 installation
- Inside `MSL_Extras` rather than `MSL_C`

Every compilation unit hits this error once = 39 "cannot be opened" errors.

---

## Error Breakdown

| Pattern | Count | Cause |
|---------|------:|-------|
| `illegal function definition` | 3,237 | Cascade from wrong time.h in prefix |
| `identifier expected` | 390 | Cascade |
| `declaration syntax error` | 195 | Cascade |
| `stat.h cannot be opened` | 39 | Deleted shim, MSL path wrong |
| `')' expected` | 39 | `time_t` unknown in wrong time.h |

| File | Count | Cause |
|------|------:|-------|
| MacTypes.h | 3,705 | Everything after broken time.h cascades into MacTypes.h |
| time.h (NetSurf's) | 156 | Wrong time.h found by prefix |
| macsurf_prefix.h | 39 | stat.h not found |

---

## The Fix

**All 3,900 errors have exactly 2 root causes, both in `macsurf_prefix.h` lines 16-17:**

### Fix A: Remove `#include <time.h>` and `#include <stat.h>` from the Prefix

These two lines must be removed entirely. They cannot be used because:
- `<time.h>` resolves to NetSurf's `utils/time.h` instead of MSL's
- `<stat.h>` doesn't exist as a standalone MSL header at that path

The types they were supposed to provide (`time_t`, `struct stat`, `mode_t`) need to come from a different source:
- `time_t`, typedef it directly in the prefix or in mac_types.h
- `mode_t`, typedef it directly in the prefix or in mac_types.h  
- `struct stat`, either recreate shims/stat.h or include `<sys/stat.h>` if MSL provides it

### Fix B: Restore mode_t and time_t Typedefs

We removed them from `mac_types.h` assuming MSL would provide them via `<time.h>` and `<stat.h>`. Since those includes don't work, we need the typedefs back:
```c
typedef long time_t;
typedef unsigned long mode_t;
```

These should go either in mac_types.h (with guards) or directly in the prefix file.

### Fix C: Decide on stat.h Strategy

Options:
1. **Recreate shims/stat.h**, the simplest, we've done this before
2. **Try `#include <sys/stat.h>`**, MSL may have stat in a `sys/` subdirectory
3. **Define struct stat in the prefix file directly**, nuclear option

---

## Prioritized Fix Order

| # | Fix | Impact |
|---|---|---:|
| **1** | Remove `#include <time.h>` and `#include <stat.h>` from prefix | **-3,900** (all errors are from these 2 lines) |
| **2** | Restore `time_t` and `mode_t` typedefs (prefix or mac_types.h) | Prevents new cascading |
| **3** | Recreate shims/stat.h with struct stat definition | Provides struct stat |

### Expected Outcome

After these 3 fixes, the error count should return to approximately **~269** (the pre-regression level) and possibly lower if the other fixes in this build (fpmath ICE bypass, C89 scope, SCN macros, inttypes cleanup) also take effect.
