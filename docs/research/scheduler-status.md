# MacSurf Scheduler, Event Loop & Window Status

Cooperative multitasking scheduler, WaitNextEvent loop, and browser window implemented in `browser/netsurf/frontends/macos9/`. Replaces the stub versions from the scaffold phase.

---

## Files Modified

### schedule.c, Timed callback scheduler

Linked list of pending callbacks sorted by absolute tick time. Follows the RISC OS `frontends/riscos/schedule.c` pattern.

| Function | Purpose |
|---|---|
| `macos9_schedule(int t, callback, void *p)` | Insert/update callback. Removes any existing entry for the same callback+param pair first. Negative `t` cancels without re-inserting. Converts ms → ticks: `ticks = (ms * 60) / 1000` |
| `macos9_schedule_run()` | Walk queue head, dispatch all callbacks with `time <= now`, free entries. Returns `true` if callbacks remain |
| `macos9_get_next_delay()` | Returns ticks until next due callback, or 15 ticks if queue empty. Used as WaitNextEvent sleep parameter |
| `macos9_get_ticks()` | Static. Returns `TickCount()` on Mac OS 9, stub value on Linux |

Timing: `TickCount()`, 1 tick = 1/60th second, monotonic from boot. Conversion: `ticks = (ms * 60) / 1000`.

Safety: callback+param dedup on insert (same as RISC OS). Queue is in consistent state before invoking any callback, since callbacks may re-enter `macos9_schedule()`.

### main.c, WaitNextEvent event loop

`macos9_poll()` is the per-iteration function called from `main()`:

1. Determine sleep time:
   - `1` tick when `macos9_fetching` is true (keep network responsive)
   - `macos9_get_next_delay()` otherwise (sleep until next callback, max 15 ticks)
2. `WaitNextEvent(everyEvent, &event, sleep_ticks, NULL)`, yields CPU
3. `macos9_dispatch_event(&event)`, switch on `event.what`
4. `macos9_schedule_run()`, always, after every event

Event handlers:

| Event | Handler | Status |
|---|---|---|
| `nullEvent` | `macos9_handle_null_event`, idle processing placeholder | Stub |
| `mouseDown` | `macos9_handle_mouse_down`, FindWindow dispatch | **Real** |
| `mouseUp` | `macos9_handle_mouse_up`, logs coordinates | Stub |
| `keyDown` / `autoKey` | `macos9_handle_key_down`, Cmd-key → MenuKey dispatch | Real |
| `updateEvt` | `macos9_handle_update`, BeginUpdate/EraseRect/EndUpdate | **Real** |
| `activateEvt` | `macos9_handle_activate`, ActivateControl/DeactivateControl on scroll bars | **Real** |
| `osEvt` | `macos9_handle_os_event`, logs subtype byte | Stub |
| `diskEvt` | `macos9_handle_disk`, logs message | Stub |

### window.c, Browser window management

Real Carbon window with scroll bars and URL bar. All gui_window_table callbacks implemented (some still stubs for optional features).

| Function | Status | Notes |
|---|---|---|
| `macos9_window_create()` | **Real** | `CreateNewWindow(kDocumentWindowClass)`, vertical + horizontal `CreateScrollBarControl()`, URL bar via `CreateStaticTextControl()`, `ShowWindow()`, linked list insert |
| `macos9_window_destroy()` | **Real** | Unlink from list, `DisposeWindow()`, free struct |
| `macos9_window_invalidate()` | **Real** | Converts `struct rect` to `Rect`, offsets by URL bar height, `InvalWindowRect()` |
| `macos9_window_get_dimensions()` | **Real** | `GetWindowBounds(kWindowContentRgn)` minus scroll bar width (15px) and URL bar height (22px) |
| `macos9_window_get_scroll()` | Real | Returns `scroll_x`/`scroll_y` from struct |
| `macos9_window_set_scroll()` | Real | Stores to struct (SetControlValue deferred) |
| `macos9_window_set_title()` | **Real** | C string → Pascal string conversion, `SetWTitle()` |
| `macos9_window_event()` | Stub | GW_EVENT dispatch deferred |
| `macos9_window_set_url()` | Stub | SetControlData deferred |
| `macos9_find_window()` | **Real** | Walks linked list, matches by WindowRef |
| `macos9_create_initial_window()` | **Real** | Public wrapper, called from `main()` after `netsurf_init()` |

### macos9.h, Updated declarations

**struct gui_window**, real fields:

```c
struct gui_window {
    WindowRef window;        /* Carbon window */
    ControlRef scroll_h;     /* horizontal scroll bar */
    ControlRef scroll_v;     /* vertical scroll bar */
    ControlRef url_field;    /* URL text field */
    struct browser_window *bw;
    int scroll_x;
    int scroll_y;
    int content_width;
    int content_height;
    struct gui_window *next; /* linked list */
};
```

Added:
- `WindowRef` / `ControlRef` typedefs (Linux stubs via `#ifdef`)
- `MACOS9_SCROLLBAR_WIDTH` (15), `MACOS9_URLBAR_HEIGHT` (22)
- `macos9_create_initial_window()`, `macos9_window_destroy()`, `macos9_find_window()` declarations

---

## Conditional Compilation

All files use `#ifdef __MACOS9__` to gate Mac Toolbox calls:

- **macos9.h**: `#include <MacWindows.h>` + `<Controls.h>` (real) vs `typedef void *WindowRef/ControlRef` (Linux)
- **schedule.c**: `TickCount()` (real) vs `stub_ticks` static variable (Linux)
- **main.c**: Mac Toolbox event types, `WaitNextEvent()`, `BeginUpdate`/`EndUpdate`/`EraseRect`, `DragWindow`/`ResizeWindow`/`TrackGoAway`, `ActivateControl`/`DeactivateControl` (real) vs local stubs (Linux)
- **window.c**: `CreateNewWindow`, `CreateScrollBarControl`, `CreateStaticTextControl`, `ShowWindow`, `DisposeWindow`, `GetWindowBounds`, `InvalWindowRect`, `SetWTitle` (real) vs local stubs (Linux)

The Makefile sets `-D__MACOS9__` so real builds get the Toolbox calls. Linux syntax checks compile against the stubs.

---

## Compilation Results

```
=== main.c ===       ✓ clean
=== window.c ===     ✓ clean
=== schedule.c ===   ✓ clean
```

Verified with `gcc -fsyntax-only -std=c99 -Wall -I include -I . -I frontends` from the NetSurf root. Zero errors.

---

## Toolbox Initialization & Menu Bar

### main(), Toolbox init sequence

Full Mac OS 9 Toolbox initialization before `netsurf_register()`:

```
MaxApplZone() → MoreMasters() → InitGraf(&qd.thePort) → InitFonts() →
InitWindows() → InitMenus() → TEInit() → InitDialogs(NULL) →
InitCursor() → FlushEvents(everyEvent, 0)
```

### main(), Initial window

After `netsurf_init()`, calls `macos9_create_initial_window()` to create the first browser window. This creates a 640×480 document window with scroll bars and URL bar.

### macos9_init_menus(), Application menu bar

| Menu | ID | Items |
|---|---|---|
| Apple (0x14) | 128 | About MacSurf..., separator, desk accessories (AppendResMenu) |
| File | 129 | New Window /N, Open Location... /L, Close /W, separator, Quit /Q |
| Edit | 130 | Undo /Z, separator, Cut /X, Copy /C, Paste /V, Select All /A |
| Go | 131 | Back /[, Forward /], Stop /., Reload /R, separator, Home |
| Help | 132 | MacSurf Help |

Menu IDs and item constants defined in `macos9.h`.

### macos9_handle_mouse_down(), FindWindow dispatch

| Part | Action |
|---|---|
| `inMenuBar` | `MenuSelect()` → `macos9_handle_menu()` → `HiliteMenu(0)` |
| `inContent` | `SelectWindow()` if not front; content routing deferred |
| `inDrag` | `DragWindow(win, event->where, &qd.screenBits_bounds)` |
| `inGrow` | `ResizeWindow(win, event->where, NULL, NULL)` |
| `inGoAway` | `TrackGoAway()` → `browser_window_destroy(bw)` or `macos9_window_destroy(gw)` |

### macos9_handle_update(), Window redraw

`BeginUpdate(win)` → `GetPortBounds(GetWindowPort(win))` → `EraseRect()` → `EndUpdate(win)`. Content drawing via plotters not yet wired.

### macos9_handle_activate(), Scroll bar activation

Finds `gui_window` via `macos9_find_window()`. Calls `ActivateControl()`/`DeactivateControl()` on both scroll bars based on active state.

### macos9_handle_key_down(), Cmd-key shortcuts

Checks `cmdKey` modifier → `MenuKey()` → dispatches to `macos9_handle_menu()`.

---

## Design Decisions

1. **No sentinel node**, Unlike RISC OS which uses a sentinel `sched_queue` struct, we use a bare `NULL` head pointer. Simpler for our case; the RISC OS sentinel exists to avoid a special case in their loop but our double-pointer walk handles it naturally.

2. **schedule_run after every event**, RISC OS only runs on null events (to avoid re-entrance from `gui_multitask`). We don't have a multitask yield function, so running after every WaitNextEvent return is safe and keeps latency low.

3. **Separate dispatch function**, Event handlers are individual functions rather than inline cases, matching the convention in CLAUDE.md ("keep Mac Toolbox calls isolated"). When real Toolbox calls are added, each handler stays self-contained.

4. **`macos9_fetching` flag**, Global bool set by fetch code (not yet implemented). When true, WaitNextEvent sleeps only 1 tick so OT notifier results are processed promptly.

5. **Window layout constants in header**, `MACOS9_SCROLLBAR_WIDTH` (15) and `MACOS9_URLBAR_HEIGHT` (22) defined in `macos9.h` so both `window.c` and future files can use them consistently.

6. **Public wrapper for initial window**, `macos9_create_initial_window()` wraps the static `macos9_window_create()` callback. Keeps the callback table function static while allowing `main()` to create the first window.

7. **WindowRef lookup**, `macos9_find_window()` walks the linked list to map a Carbon `WindowRef` back to a `gui_window`. Used by event handlers in `main.c` that receive a `WindowRef` from the event record.

---

## What's Not Here Yet

- No content drawing via plotters in update handler (EraseRect only)
- No scroll bar value tracking (SetControlValue/GetControlValue)
- No URL bar text updates (SetControlData)
- No content dimension recalculation after ResizeWindow
- No Open Transport notifier integration
- No mouse region for `WaitNextEvent` cursor tracking
- No browser_window content routing (mouse clicks, key events)
- No About dialog implementation
