# CW8 Build Attempt, Current Error Analysis

**303 errors. 8 root causes.**

## Root Cause 1, `time_t` unknown (~150 cascade errors)

`url_db.h` line 28 does `#include <time.h>` but CW8 finds a wrong
version. The old `browser:netsurf:utils:time.h` must still be on the
Mac filesystem. It was renamed to `ns_time.h` in the repo but if the
old file wasn't deleted from the Mac, it shadows MSL's `<time.h>`.

Cascades into: `dt_global_history.c` (48+), `cookie_manager.c` (21+),
`dt_hotlist.c` (9+), `fetch.c`, `browser_window.c`,
`browser_history.c`.

**Fix:** Delete `browser:netsurf:utils:time.h` from the Mac.

## Root Cause 2, `strchr`/`strstr`/`strrchr` return int (~15 errors)

`dt_download.c`, `dt_searchweb.c`, `form.c`, `strchr` returning
`int` instead of `char *`. This means `<string.h>` isn't being
found. Same shadowing issue, check if any stale `string.h` exists
in user paths.

**Fix:** Confirm libhubbub's old `string.h` was deleted from the Mac
(renamed to `hub_string.h`).

## Root Cause 3, `dt_treeview.c` for-scope + NSLOG cascade (~50 errors)

Lines 2004 (designated init), 2272 and 2362 (`for (struct
treeview_node *n = ...)`), plus NSLOG expansion issues at 2301,
2305, 2394, 2427, 2431 where `nslog_log(...)` isn't terminated
with `;`.

**Fix:** Hoist for-scope decls, fix designated init, check NSLOG
expansion produces valid statements.

## Root Cause 4, `css_utils.c` function not parsed (~13 errors)

`css__number_from_string` parameters (`data`, `len`, `consumed`,
`int_only`) all undefined. The function signature at line 31 uses
`css_fixed` return type which depends on includes. The updated
`css_utils.c` with added includes may not be on the Mac.

**Fix:** Confirm `css_utils.c` on the Mac has the added
`#include <libcss/errors.h>` and `#include <libcss/fpmath.h>`.

## Root Cause 5, `hashmap.c` for-scope (~8 errors)

Lines 235, 238: `for (uint32_t bucket = ...)` and
`for (hashmap_entry_t *entry = ...)`.

**Fix:** Hoist declarations.

## Root Cause 6, `frames.c` VLA (~2 errors)

Line 334-335: `int widths[bw->cols][bw->rows]`, variable-length
array. C89 doesn't support VLAs.

**Fix:** Replace with `malloc`/`free` or a fixed-size array.

## Root Cause 7, `flex.c` mixed declarations (~4 errors)

Lines 152, 164: `css_fixed grow_num = ...` and
`css_fixed shrink_num = ...` after statements.

**Fix:** Hoist to function top.

## Root Cause 8, `computed.c` mixed declaration (~3 errors)

Line 760: `uint8_t ret = get_width(...)` after statements.

**Fix:** Hoist.

## Root Cause 9, 7 files still "could not find"

Same 7: `cache-control.c`, `challenge.c`, `content-disposition.c`,
`content-type.c`, `cssh_css.c`, `cssh_select.c`, `dump.c`.
Plus `generics.c`, `hints.c`.

## Fix priority

| # | Cause | Errors | Fix type |
|---|---|---|---|
| 1 | time.h shadow | ~150 | Mac: delete old file |
| 2 | string.h shadow | ~15 | Mac: delete old file |
| 3 | dt_treeview.c C89 | ~50 | Code fix |
| 4 | css_utils.c | ~13 | Mac: verify file |
| 5 | hashmap.c for-scope | ~8 | Code fix |
| 6 | frames.c VLA | ~2 | Code fix |
| 7 | flex.c mixed decl | ~4 | Code fix |
| 8 | computed.c mixed decl | ~3 | Code fix |
| 9 | 9 files not found | blocks | Mac: add files |
