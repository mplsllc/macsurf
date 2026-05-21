# MacSurf Window Framework, Architecture Research

**Round:** fixes161 (research only, no code changes).
**Date:** 2026-04-22.
**Branch:** `v0.3-rendering`.
**Last shipped:** fixes160 (`e5e59ad`).
**Scope:** the Mac OS 9 frontend's window framework, `window.c`, `main.c` (event dispatch / update only), `macos9.h`, `plotters.c` (read-only, to document redraw assumptions), `macos9_wheel.c`, `macsurf_debug_log.c`. Everything under `browser/libcss/`, `browser/netsurf/content/`, and `proxy/` is explicitly out of scope.

The goal is not to ship a fix. The goal is to produce an accurate, source-backed picture of what the window framework actually does today, as distinct from what its ~20 patch rounds describe themselves as doing, and from that picture derive a refactor plan that each subsequent round can execute without re-deriving its own local model of the subsystem.

---

## Section 1, `gui_window` state inventory

`struct gui_window` is defined in [macos9.h](../../browser/netsurf/frontends/macos9/macos9.h). 22 fields (including the list linkage). For each field: purpose, writer set, reader set, invariants the code assumes at read time, and places where the code breaks those invariants in the real tree today.

Writer/reader sets were derived by grep across the scope files; they do NOT cover any NetSurf-core code path, which is out of scope. When NetSurf core calls into a gui_window_table callback and the callback mutates state, that callback is named as the writer (e.g. `macos9_gw_event` for `content_width`).

### 1.1 `WindowRef window`

Purpose: Carbon window handle. The physical window the user sees.

**Writers:**
- `macos9_window_create`, `CreateNewWindow(…, &gw->window)` stores the returned handle.
- `macos9_window_destroy`, nulls after `DisposeWindow`.

**Readers:** almost every function in `window.c` and `main.c`'s event handlers (`SetPortWindowPort(gw->window)`, `InvalWindowRect(gw->window, …)`, etc.). Also read by plotters indirectly (plotters operate on the current port, which the update handler sets from `gw->window`).

**Invariants assumed:**
- Non-NULL once `macos9_window_create` returns.
- Valid until `macos9_window_destroy` nulls it (every mutator re-nulls instead of freeing in place).
- Owns all six controls + `url_te` (disposed implicitly via `DisposeWindow` per `macos9_window_destroy` comment).

**Current inconsistencies:**
- `macos9_handle_update` checks `gw->window == NULL` as a guard. If an update event can dispatch with a destroyed window, the guard is necessary; in practice `macos9_windows` list removal in `macos9_window_destroy` happens *before* `DisposeWindow`, so a race window exists between the list-remove and the Window Manager's completion. Low-probability path; harmless because the guard fires.
- During shutdown, `macos9_quitting = true` is the primary gate in `macos9_handle_update`; `gw->window == NULL` is a secondary gate. Two overlapping guards is fine, just over-indexed.

### 1.2 `ControlRef back_btn`, `forward_btn`, `reload_btn`, `home_btn`

Purpose: four toolbar push buttons.

**Writers:**
- `macos9_window_create`, `NewControl(...)` for each.
- `macos9_window_destroy`, nulls before `DisposeWindow` (per fixes146 hardening; Control Manager state might be read through dangling handles during window destruction).

**Readers:**
- `macos9_window_update_button_states`, `HiliteControl` + `Draw1Control` per button.
- `macos9_handle_activate`, `HiliteControl(ctrl, 255)` + `Draw1Control` when window deactivates.
- `macos9_handle_mouse_down`, `FindControl` return is compared against these pointers to decide which navigation action to invoke (`back`, `forward`, `reload`, `home`).

**Invariants assumed:**
- Non-NULL after creation, until destruction.
- Owned by `gw->window`; disposed transitively by `DisposeWindow`.
- `FindControl` returns one of these four (or one of `vscroll`/`hscroll`, or NULL), identity comparison is the dispatch.

**Current inconsistencies:**
- **Bounds recomputation on resize: missing.** `macos9_window_layout` recomputes `gw->toolbar_rect`, `gw->url_rect`, `gw->content_rect`, `gw->status_rect`, and calls `MoveControl`/`SizeControl` for the two scroll bars. **It does not call `MoveControl`/`SizeControl` on the four buttons.** On a resize, the buttons stay at their initial creation bounds, even though `toolbar_rect` has been recomputed against the new window width. For buttons at the left edge of a two-row toolbar (fixes153), this is invisible, their bounds are in the left column, which doesn't depend on window width. It would break if any button were right-anchored, or if `MACOS9_BTN_Y` were resize-dependent. **This is a latent bug waiting for any layout change.**
- `macos9_window_update_button_states` force-draws each button via `Draw1Control` after `HiliteControl` (fixes151). This is correct but documents an invariant that `HiliteControl` alone does NOT immediately repaint Appearance controls, any future code that calls `HiliteControl` directly on these ControlRefs without a following `Draw1Control` will regress the visible-state lag bug.

### 1.3 `ControlRef vscroll`, `hscroll`

Purpose: vertical and horizontal scroll bars. Created with `kControlScrollBarProc` (384, non-live variant, per fixes159) to dodge the hardware-specific crash in proc 386's live-track CDEF.

**Writers:**
- `macos9_window_create`, `NewControl` + `ShowControl` (defensive, per fixes154).
- `macos9_window_layout`, `MoveControl` + `SizeControl` on every layout pass (resize or create).
- `macos9_window_update_scrollbars`, `SetControlMinimum` / `SetControlMaximum` / `SetControlValue` / `HiliteControl` / `Draw1Control`.
- `macos9_window_scroll_to`, `SetControlValue` + `Draw1Control` (fixes150 wired `Draw1Control` to the scroll path).
- `macos9_handle_activate`, `HiliteControl(ctrl, 255) + Draw1Control` on deactivate.
- `macos9_window_destroy`, null before `DisposeWindow`.

**Readers:**
- `macos9_window_handle_scrollbar_click`, `GetControlValue`, `GetControlMinimum`, `GetControlMaximum`, `(**ctrl).contrlOwner`, `(**ctrl).contrlVis` (the last two for diagnostic logging; dereferencing the ControlRef is done to catch dangling-handle crashes).
- `macos9_handle_mouse_down`, `FindControl` identity comparison.
- `macos9_handle_activate`, identity check for HiliteControl grey-out.

**Invariants assumed:**
- Non-NULL from creation through destruction.
- `contrlVis != 0` (`ShowControl` called at creation).
- `GetControlValue(ctrl)` returns the authoritative scroll position in pixels after `TrackControl` returns (this is the fixes147 + fixes159 contract, the non-live CDEF leaves the final thumb value in the control's value field on mouseup).
- `GetControlMaximum(ctrl)` equals `content_height - viewport_height` clamped to `[0, 32000]` (pinned to `SInt16` range in `update_scrollbars`).

**Current inconsistencies:**
- **Two competing authorities on scroll position: `GetControlValue(ctrl)` and `gw->scroll_y`.** `macos9_window_scroll_to` writes both in sequence (first `gw->scroll_y = new_y`, then `SetControlValue(ctrl, (short)new_y)`). If the order is ever reversed, or if an intermediate `macos9_window_update_scrollbars` clamps `gw->scroll_y` between the two writes, the two values drift. The fixes156 log line `sb_upd CLAMP scroll (ox,oy)->(nx,ny)` exists specifically to detect this drift, which tells us it has happened at least once.
- **`SInt16` truncation.** `(short)new_y` in `macos9_window_scroll_to` truncates silently if `new_y > 32767`. The clamp in `update_scrollbars` sets `cmax_y` via `max_y > 32000 ? 32000 : max_y` (good), but `scroll_to` does not apply the same 32000-pixel ceiling to the `SetControlValue` argument. A `content_height > 32767` page scrolled to the bottom puts a garbage value in the control's value field. Bound has not been reported in practice because MacTrove's content height is ~800 px, but the bug exists.
- **Live-tracking contract.** Per fixes147 / fixes159 comments, `TrackControl(ctrl, pt, NULL)` with the non-live CDEF leaves the final drag value in the control on mouseup. This is correct for thumb drag. For arrow/page parts, `TrackControl`'s return value is the `ControlPartCode` that was hit, but the existing code **does not use** `TrackControl`'s return; it branches on the `cpart` captured by `FindControl` *before* `TrackControl` was called. If the user clicks on the up-arrow and then drags into the page-up area before releasing, `cpart` stays `kControlUpButtonPart` even though the user's final intent was page-up. Minor, most users release on the initial part, but it's a behavior mismatch with how Appearance scroll bars are supposed to work.
- **No per-tick update during drag.** Non-live CDEF means the content does not scroll during thumb drag; it snaps at mouseup. This is a documented trade-off in fixes159's CLAUDE.md entry, not a bug.

### 1.4 `struct browser_window *bw`

Purpose: the NetSurf browser_window handle. Most of the content-area functionality routes through NetSurf core via this pointer.

**Writers:**
- `macos9_window_create`, stored from the constructor argument.

**Readers:**
- `macos9_window_navigate`, `browser_window_navigate(gw->bw, …)`.
- `macos9_window_back` / `forward` / `reload` / `home`, `browser_window_history_*`.
- `macos9_window_address_bar_submit`, `browser_window_get_url` (via the Escape-key path).
- `macos9_window_update_extents`, `browser_window_get_extents(gw->bw, …)`.
- `macos9_window_update_button_states`, history availability queries.
- `macos9_handle_update`, `browser_window_redraw_ready(gw->bw)` + `browser_window_redraw(gw->bw, …)`.
- `macos9_handle_key_down`, `browser_window_get_url` on Escape (URL-field mode).
- `macos9_gw_event`, other gui_window_table callbacks, receive `gw`, read `gw->bw` if needed.

**Invariants assumed:**
- May be NULL (initial-window path with no OT has `bw == NULL`; `File → New Window` path creates a bw-less window and then calls `macos9_window_home` which wants a bw → early-returns).
- If non-NULL, valid until window destruction.
- NetSurf core may dispatch gui_window_table callbacks *during* `browser_window_create`, before control returns from the initial `macos9_window_create`. The window's `bw` field has been written by the constructor by that point, but only because the constructor stores it before any callbacks can reach back.

**Current inconsistencies:**
- The `bw == NULL` branch is handled ad-hoc in each reader, sometimes with an early return, sometimes with a fallthrough that silently no-ops. There is no single sentinel for "this window doesn't have a bw yet / anymore". A future refactor should make this explicit.
- `macos9_window_home` early-returns if `bw == NULL`, which means `File → New Window` (before OT is available) opens a window and then does nothing. If OT is live, `macos9_handle_menu` `ITEM_FILE_NEW` calls `macos9_create_initial_window` which does NOT create a bw, the window exists but has no content pipeline. Divergent behavior from the initial-window path, which creates the bw via `browser_window_create` before the window constructor runs.

### 1.5 `TEHandle url_te`

Purpose: per-window TextEdit handle backing the URL bar. Separate from `gw->bw`'s content-area TextEdit (there isn't one; the content area is drawn via plotters, not TE).

**Writers:**
- `macos9_window_create`, `TENew(&te_rect, &te_rect)`, then `TESetText` with `MACSURF_HOME_URL`, then `TEActivate`.
- `macos9_window_destroy`, `TEDispose` + null before `DisposeWindow` (fixes146).
- `macos9_window_resize`, rewrites `destRect` / `viewRect` in the TERec through handle deref + `TECalText`.
- `set_url_te_text` (static helper), `TESetText` + `invalidate_url`.
- `macos9_window_address_bar_submit`, `TEGetText` (read, not mutate, but the handle is dereferenced).
- `macos9_handle_key_down`, `TEKey` for character typing; also `TESetText` on Escape to restore the original URL.
- `macos9_window_te_activate_url`, `TEActivate` + flip flag.
- `macos9_window_te_deactivate_url`, `TEDeactivate` + flip flag.
- `macos9_windows_te_idle`, `TEIdle` every event-loop pass for every window whose flag is true.
- `macos9_handle_mouse_down`, `TEClick` on URL-rect hit.
- `macos9_handle_update`, `TEUpdate(&gw->url_rect, gw->url_te)` inside `draw_url_bar`.
- `macos9_handle_activate`, `TEActivate` / `TEDeactivate` when the window activates / deactivates.

**Invariants assumed:**
- Non-NULL after creation unless `TENew` failed (checked in constructor with NULL guard).
- `SetWRefCon` has been called *before* `TENew`, otherwise TextEdit crashes with `dsMemWZErr` on a garbage WRefCon (explicit CLAUDE.md gotcha).
- `destRect` / `viewRect` in the TERec match the current `url_rect` minus `MACOS9_URL_INSET`. `macos9_window_resize` maintains this; no other code path does. Creation-time setup relies on `compute_url_te_rect(&gw->url_rect, &te_rect)` and `TENew(&te_rect, &te_rect)`.
- The current `GrafPort` is the window's port (`SetPortWindowPort(win)`) when any TE call happens. Violated without this; TE writes through the wrong port and crashes or mis-draws.

**Current inconsistencies:**
- **`TEUpdate` clip mismatch.** `draw_url_bar` calls `TEUpdate(&gw->url_rect, gw->url_te)`, passing the **outer** `url_rect` as the update rect. But `TENew` was given `te_rect` (the inset version). TextEdit's internal line geometry is relative to its `viewRect`, not to the rect passed to `TEUpdate`, so this works in practice because `TEUpdate`'s rect parameter is a clip boundary, not the TextEdit's own rect. Still, documenting "the rect passed to `TEUpdate` is a clip, not a positional reference" is worth doing explicitly; the current code is silently correct but visually confusable with a bug.
- **`url_field_active` and TE activation drift.** The flag `url_field_active` is the code's single source of truth for "is the URL field focused". `TEActivate` / `TEDeactivate` have their own internal state (the caret blinks or doesn't). There are paths where these can diverge:
  - `macos9_window_create` sets `url_field_active = true` AND calls `TEActivate` (fixes150). Consistent.
  - `macos9_handle_activate` on window-deactivate calls `TEDeactivate` but **does not** clear `url_field_active`. Re-activating the window (fixes153) sets `url_field_active = true` and calls `TEActivate`. Net: the flag can be stale between deactivate and re-activate, but since nothing routes keystrokes while the window is inactive (deactivated windows aren't front), the drift is invisible.
  - `macos9_handle_mouse_down` on a non-URL control click calls `macos9_window_te_deactivate_url(gw)` which clears the flag AND calls `TEDeactivate`. Consistent.
  - Empty-content click (mouse down in content area, no control hit) also clears the flag and calls `TEDeactivate`. Consistent.
- **No explicit null guard on `gw->url_te` in `macos9_handle_key_down`.** The function checks `gw->url_field_active && gw->url_te != NULL` as the entry gate, but then the Escape path sets text without re-checking `gw->url_te` (yes it does, via `gw->url_te != NULL` in the inner `if`). Fine.

### 1.6 `bool url_field_active`

Purpose: focus flag. When true, keystrokes route to `TEKey`; when false, they route to `handle_scroll_key`.

**Writers:**
- `macos9_window_create`, set `true` at window creation (fixes150 default).
- `macos9_window_te_activate_url`, set `true`.
- `macos9_window_te_deactivate_url`, set `false`.
- `macos9_handle_activate`, set `true` on window activation if `url_te != NULL` (fixes153 restoration).
- `macos9_handle_mouse_down` (URL rect click), set `true` via `te_activate_url`.
- `macos9_handle_mouse_down` (any control click, empty content click), set `false` via `te_deactivate_url`.

**Readers:**
- `macos9_handle_key_down`, the one branch that matters.
- `macos9_windows_te_idle`, only idles the TE if flag is true.
- `macos9_handle_mouse_down`, decides whether to call `te_deactivate_url` before dispatching a control click.

**Invariants assumed:**
- One-window focus model: the flag is per-window, but no cross-window "which window's URL field is focused" arbiter exists.
- `true` implies `url_te != NULL` (enforced at every write site).
- `false` does not imply `url_te` has been `TEDeactivate`'d, just that the code won't route keys to it. This is a lossy invariant; the drift is harmless but ugly.

**Current inconsistencies:**
- **Multi-window focus drift.** If two windows exist and both were activated in sequence, both may have `url_field_active == true`. Only `FrontWindow()` receives keystrokes, so the flag on the non-front window doesn't route anything, but `macos9_windows_te_idle` iterates the entire `window_list` and calls `TEIdle` on every window whose flag is true. Net: the non-front window's caret continues blinking. Minor cosmetic issue.
- **Conservative default (fixes153).** Every window activation sets `url_field_active = true` as long as `url_te != NULL`. This clobbers whatever the user had previously focused (e.g. clicking on the content area cleared the flag; then switching apps and coming back re-sets it to true, stealing focus back to the URL bar). Acceptable because the user gets a visible caret in the URL bar after app switch, but it's worth documenting as a deliberate focus-steal.

### 1.7 `int scroll_x`, `scroll_y`

Purpose: current scroll offset in document pixels. The offset from the top-left of the document to the top-left of the visible viewport (so `browser_window_redraw` is called with `(content_rect.left - scroll_x, content_rect.top - scroll_y)` as the content origin).

**Writers:**
- `macos9_window_create`, initialize to 0.
- `macos9_window_update_scrollbars`, clamps via `max_x`/`max_y` and rewrites if out of range.
- `macos9_window_scroll_to`, authoritative setter. Clamps to `[0, max_x]` / `[0, max_y]` then writes.
- `macos9_window_scroll_by`, just calls `scroll_to` with summed offsets.
- `macos9_gw_set_scroll`, gui_window_table callback from NetSurf core; dispatches to `scroll_to`.

**Readers:**
- `macos9_handle_update`, passes `-scroll_x`, `-scroll_y` as content origin offset to `browser_window_redraw`.
- `macos9_window_update_scrollbars`, writes to control value.
- `macos9_window_scroll_to`, reads for no-op detection.
- `macos9_gw_get_scroll`, gui_window_table callback; reads.

**Invariants assumed:**
- `0 <= scroll_x <= max_x`, `0 <= scroll_y <= max_y` where `max_* = content_* - viewport_*` clamped to non-negative.
- `scroll_y >= 0` always (enforced in `scroll_to`).
- `GetControlValue(vscroll) == scroll_y` after any consistent state (enforced at every scroll write: `scroll_to` writes both).

**Current inconsistencies:**
- **`content_height` can change between a `scroll_to` and the next `update_scrollbars`.** If a `GW_EVENT_UPDATE_EXTENT` fires between them with a shorter new `content_height`, `scroll_y` may still point past the new end of content. `update_scrollbars` catches this by clamping and logging `sb_upd CLAMP`, but the `SetControlValue` in the *earlier* `scroll_to` has already put the stale value in the control. Until the next paint cycle, the control thumb sits outside the new valid range. Cosmetic in practice; `update_scrollbars` fixes it within one event-loop pass.
- **No distinct "document coordinates" vs "screen coordinates" type.** `scroll_x` is a plain `int`. So is `gw->content_rect.left`. The redraw handler subtracts them as if they were compatible, which they are, but there's no compile-time enforcement of "don't pass a document-Y to a function expecting a viewport-Y". The code convention is implicit.
- **Part-code-driven vs control-value-driven scroll.** `handle_scrollbar_click` applies a delta for arrow/page parts (part-code-driven; explicit `MACOS9_KEY_SCROLL_STEP` or `view_h - 32`) and reads the control value for thumb-drag. These two paths compute the new scroll position through different arithmetic. Divergence risk: if `MACOS9_KEY_SCROLL_STEP` changed while the user was dragging, the next arrow click would step by the new value even though the thumb is still at the old control value. Not a real bug (step isn't runtime-configurable) but a sign that the two authorities aren't unified.

### 1.8 `int content_width`, `content_height`

Purpose: cached extents of the document content, in pixels.

**Writers:**
- `macos9_window_create`, initialize to 0.
- `macos9_window_update_extents`, `browser_window_get_extents(gw->bw, false, &w, &h)`, assigns on NSERROR_OK only.

**Readers:**
- `macos9_window_update_scrollbars`, compute `max_x`, `max_y` = content - viewport.
- `macos9_window_scroll_to`, compute clamp limits.
- `handle_scroll_key`, compute `max_y` for the End key.

**Invariants assumed:**
- Non-negative.
- Monotonic with respect to document content (but not enforced, if the document shrinks, these can shrink).
- Reflects the latest `browser_window_get_extents` result.

**Current inconsistencies:**
- **Staleness window between layout passes.** The initial `GW_EVENT_UPDATE_EXTENT` fires before late layout passes (CSS resolve, font fallback, flex reflow) finish. `update_extents` runs, stores a shorter `content_height` than the final, and `update_scrollbars` disables the vertical scroll bar because `max_y == 0`. The user has to manually resize the window to force `macos9_window_resize` to re-call `update_scrollbars`. This was the fixes160 fix: also call `update_extents` on `GW_EVENT_STOP_THROBBER`. That closes the staleness window for "load completed" but does not close it for "content reflowed later" (e.g. image loaded, text wraps differently). The invariant is: `content_height` is accurate at the most recent `update_extents` call.
- **No explicit unit.** The comment says "document pixels". NetSurf's `browser_window_get_extents` returns values in CSS pixels, which on MacSurf's zoom=1 is document pixels. Not self-documenting.

### 1.9 `Rect toolbar_rect`, `url_rect`, `content_rect`, `status_rect`

Purpose: pre-computed layout rectangles within the window's local coordinate system. Recomputed on every `macos9_window_layout` call.

**Writers:**
- `macos9_window_layout`, `SetRect(&gw->*_rect, ...)` for all four, every call.

**Readers:**
- `toolbar_rect`, **no direct reader anywhere.** Computed but unused; the two-row toolbar (fixes153) recomputes its geometry inside `macos9_window_layout` locally and doesn't re-read `toolbar_rect`. Dead field.
- `url_rect`, `compute_url_te_rect` (static helper called from `resize` and `create`), `draw_url_bar` (called from `handle_update`), `macos9_handle_mouse_down` (hit test), `invalidate_url`.
- `content_rect`, `content_view_width`, `content_view_height` (helpers for scroll math), `handle_update` (clip + redraw offset), `invalidate_content` (Inval target), `macos9_gw_get_dimensions` (reports viewport size to NetSurf core), `handle_scroll_key` (viewport size for page step), `handle_scrollbar_click` (viewport size for page step).
- `status_rect`, `draw_status_bar`, `invalidate_status`.

**Invariants assumed:**
- All in window-local coordinates (origin at window port origin, which is `(0,0)` when `SetPortWindowPort` is called).
- `SetPortWindowPort(gw->window)` has been called before any code dereferences these with QuickDraw calls (`EraseRect`, `FrameRect`, `InvalWindowRect`).
- Non-overlapping, except for the scroll bar gutter (which has no explicit rect) and the status/scrollbar corner (undefined).
- `content_rect` is computed with a non-negative size (`win_w - MACOS9_SCROLLBAR_WIDTH` ≥ 1), not actually enforced; tiny windows would produce inverted or zero rects. `content_view_width` / `_height` clamp to ≥1 after the fact, which papers over the pathological case.

**Current inconsistencies:**
- **`toolbar_rect` is dead.** Could be removed. Its original purpose was probably "invalidate the toolbar on activate/deactivate"; current code uses `invalidate_all` instead.
- **URL rect click detection vs TE internal rect.** The mouse-down handler checks `point_in_rect(local, &gw->url_rect)`. If true, it calls `TEClick(local, ...)`. TE's internal `viewRect` is the inset version (`compute_url_te_rect`). A click at `url_rect.left + 1` (inside `url_rect`, outside `te_rect`) goes to `TEClick` but TextEdit may or may not accept it as a valid click position depending on its own rect bookkeeping. In practice TE appears to accept clicks anywhere near its `viewRect` (it's forgiving), so this isn't visibly broken, but the two rect systems aren't formally aligned.
- **Four rects, one bounds source.** All four derive from `GetWindowBounds(gw->window, kWindowContentRgn, &content)`. Any single call to `macos9_window_layout` recomputes them consistently. There is no way to update one without the others, which is a good property.
- **Status rect lives below content, not between content and scroll bars.** The layout computes `hscroll_top = win_h - MACOS9_SCROLLBAR_WIDTH`, `content_bottom = hscroll_top - MACOS9_STATUS_HEIGHT`. So status sits ABOVE the horizontal scrollbar. Non-standard, most Mac apps put the status bar below everything, with scrollbars not extending to the edge. This is a Platinum stylistic choice, not a bug.
- **Scroll bar rects: implicit.** The scroll bar controls' bounds are managed directly on the ControlRef (`MoveControl` / `SizeControl` in `macos9_window_layout`) and are NOT stored in `gw`. So there are two distinct "where is the vertical scroll bar" authorities: the `ControlRecord` struct and the bounds implied by `content_rect.right` + `MACOS9_SCROLLBAR_WIDTH`. No divergence path today, but a refactor that wanted to, say, log "where are the scroll bar rects?" would have to dereference the ControlRef (as `handle_scrollbar_click` does for diagnostics).

### 1.10 `bool needs_reformat`, `reformat_in_progress`

Purpose: deferred-flag pattern for window resize. `needs_reformat` is set on grow-box release, and `macos9_windows_process_deferred` (called every event-loop pass) drains it by calling `browser_window_schedule_reformat`. `reformat_in_progress` is a re-entrancy guard that logs and early-returns if another reformat starts while one is running.

**Writers:**
- `needs_reformat`:
  - `macos9_window_create`, false.
  - `macos9_window_resize`, true (after `macos9_window_layout` and scrollbar updates).
  - `macos9_windows_process_deferred`, false (at the top of the work block).
- `reformat_in_progress`:
  - `macos9_window_create`, false.
  - `macos9_windows_process_deferred`, true (before the reformat call), false (after).

**Readers:**
- `macos9_windows_process_deferred`, reads both, every event-loop pass, per window.

**Invariants assumed:**
- `process_deferred` runs before `WaitNextEvent` every pass (not before every event, before every `macos9_poll`).
- `needs_reformat` is idempotent: setting it multiple times is fine; it drains at most once per poll cycle.
- `reformat_in_progress` prevents re-entrance via the deferred queue, but **not** via direct calls to `browser_window_schedule_reformat`. The pattern trusts that no code path calls reformat synchronously, which is true today because the direct-call paths have been removed (fixes146 era).

**Current inconsistencies:**
- **`process_deferred` runs per-poll, not per-event.** A burst of resize events (the user dragging the grow box) sets the flag repeatedly; each poll-pass drains it once. The design assumes the drain is cheap relative to resize events, which is almost true: one `browser_window_schedule_reformat` per poll is N orders of magnitude less work than one per resize event. If the user resizes very fast (a modern mouse can fire mouseMoved in a tight loop; OS 9 doesn't have mouseMoved but it does have `GrowWindow`'s internal polling), the deferred pass is still called only once per poll. Good.
- **Flag is cleared before the reformat call starts, not after.** The order is: set `needs_reformat = false`, set `reformat_in_progress = true`, call reformat, set `reformat_in_progress = false`. If reformat internally sets `needs_reformat = true` (via an `invalidate_all` → event-loop → resize pattern? unclear, would need to audit NetSurf core), the flag stays true for the next pass. OK.

### 1.11 `char status[128]`

Purpose: current status-bar text.

**Writers:**
- `macos9_window_create`, `"Ready"`.
- `set_status_text` (static helper), `strncpy` with truncation to 127 chars + terminator.
- Called from `macos9_window_navigate` (`"Loading..."`), `macos9_gw_set_status` (NetSurf-driven), `macos9_gw_event`'s `GW_EVENT_STOP_THROBBER` (`"Done"`), `GW_EVENT_START_THROBBER` (`"Loading..."`).
- `main.c` also has a fallback write in the initial-window OT-disabled path (`"No TCP/IP - fetches disabled"` via `strncpy` directly).

**Readers:**
- `draw_status_bar` (in `main.c`), `DrawText`.

**Invariants assumed:**
- Null-terminated.
- Length strictly < 128.
- The main.c fallback and the window.c `set_status_text` helper use the same truncation logic. They do.

**Current inconsistencies:**
- **Duplicate write paths.** `main.c`'s fallback bypasses `set_status_text`. Two writers with the same semantics is a minor smell; a single entry point would be cleaner.

### 1.12 `struct gui_window *next`

Purpose: singly-linked list linkage. Head is static `window_list`.

**Writers:**
- `macos9_window_create`, `gw->next = window_list; window_list = gw;`.
- `macos9_window_destroy`, unlinks via the `**p` idiom.

**Readers:**
- `macos9_find_window`, linear scan.
- `macos9_windows_te_idle`, iterate all.
- `macos9_windows_process_deferred`, iterate all.

**Invariants assumed:**
- Non-cyclic.
- Every entry is reachable from `window_list`.
- A window is in the list from `macos9_window_create` return through `macos9_window_destroy` return.

**Current inconsistencies:**
- No lock / no per-window flag, fine because OS 9 is cooperative; no preemptive threads ever touch this list.

---

## Section 2, Control lifecycle

Per-control walkthrough. Each control is a `ControlRef` owned by `gw->window`, created by `NewControl`, disposed implicitly by `DisposeWindow`.

### 2.1 `back_btn`, `forward_btn`, `reload_btn`, `home_btn`

- **Created:** [window.c macos9_window_create](../../browser/netsurf/frontends/macos9/window.c), 4 × `NewControl(gw->window, &btn_rect, title, true, 0, 0, 1, kControlPushButtonProc, (long)gw)`. Bounds explicitly computed from `MACOS9_BTN_GAP`, `MACOS9_BTN_WIDTH`, `MACOS9_BTN_Y`, `MACOS9_BTN_HEIGHT` constants. First button at `x = MACOS9_BTN_GAP` (4), successive buttons at `x += MACOS9_BTN_WIDTH + MACOS9_BTN_GAP` (48 + 4 = 52 pixels later).
- **Moved/resized:** **never after creation**. Not touched by `macos9_window_layout`. Constants are absolute, not window-relative.
- **Values set:** never. Push buttons don't have meaningful values (0/1).
- **Hilite set:** `macos9_window_update_button_states`, `HiliteControl(ctrl, 0)` or `HiliteControl(ctrl, 255)` based on history availability + content availability. `macos9_handle_activate` on deactivate, `HiliteControl(ctrl, 255)` + `Draw1Control`.
- **Drawn:** explicitly via `Draw1Control` after every `HiliteControl` write (fixes151 added these). Implicitly via `DrawControls(win)` in `macos9_handle_update`'s update handler.
- **Hit-tested:** `FindControl` in `macos9_handle_mouse_down` returns the ControlRef. Identity comparison picks the navigation action.
- **Disposed:** `macos9_window_destroy` nulls the refs first, then `DisposeWindow` frees the control internally.

**Gaps:**
- **Bounds not updated on resize.** Documented above in §1.2. Works only because buttons happen to be left-anchored.
- **No disabled-state repaint at deactivate + re-activate round-trip.** `handle_activate` on deactivate sets hilite to 255; on re-activate, `update_button_states` overwrites with the actual availability hilite. If `update_button_states` ran before `handle_activate` for some reason (not reachable today, but the ordering is implicit), the button would look wrong.

### 2.2 `vscroll`, `hscroll`

- **Created:** `NewControl(...)` with `kControlScrollBarProc` (proc 384, non-live, fixes159). Initial bounds computed from `bounds` (the window's outer rect) with specific `-1`/`+1` / `+2` offsets to make the control overlap the window frame correctly (a common Mac idiom).
- **Moved/resized:** `macos9_window_layout` explicitly calls `MoveControl` + `SizeControl` every call. Uses the current window content-rect bounds, not the initial bounds.
- **Values set:** `macos9_window_update_scrollbars` (extent change, new content, scroll event) and `macos9_window_scroll_to` (programmatic scroll) set `SetControlValue`, `SetControlMinimum`, `SetControlMaximum`. `HiliteControl` based on whether `max_y > 0` / `max_x > 0` (grey-out empty-scroll).
- **Drawn:** `Draw1Control` after every value/hilite change (fixes142, fixes150). `ShowControl` defensive call at creation (fixes154). Implicit `DrawControls` in the update handler.
- **Hit-tested:** `FindControl` in `macos9_handle_mouse_down`. The `ControlPartCode` distinguishes thumb, arrows, page regions. Dispatches to `macos9_window_handle_scrollbar_click`.
- **Disposed:** null + implicit via `DisposeWindow`.

**Gaps:**
- **Dual-authority position.** Documented in §1.3.
- **No post-`TrackControl`-return check that the control's value makes sense.** If the control value is unexpectedly < 0 or > max, the dispatch into `scroll_to` clamps, but a diagnostic opportunity exists to catch "CDEF returned a value outside its min/max". No logging today.
- **`FindControl`-to-`TrackControl` gap.** Between `FindControl` returning a cpart and `TrackControl` being called, code in `macos9_handle_mouse_down` executes `macsurf_debug_log_writef` (two calls). Extremely unlikely to matter, but worth noting that `TrackControl`'s contract is "call immediately after `FindControl`" in classic Mac code.

### 2.3 `url_te`

Not a ControlRef, a TEHandle, but fills the "URL-bar widget" role.

- **Created:** `TENew(&te_rect, &te_rect)` after `SetPortWindowPort`, `SetWRefCon`, `TextFont/TextSize/TextFace`. The order is critical per the TE-dsMemWZErr gotcha.
- **Moved/resized:** `macos9_window_resize` dereferences the TEHandle, rewrites `destRect` and `viewRect` directly in the TERec, then calls `TECalText`. This is the classical Mac TE resize idiom.
- **Values set:** `TESetText` from `set_url_te_text`, `TESetText` on Escape in `macos9_handle_key_down`, `TESetText` at creation.
- **Drawn:** `TEUpdate(&gw->url_rect, gw->url_te)` inside `draw_url_bar`, called from `macos9_handle_update`'s update handler. Framed with `FrameRect(&gw->url_rect)` beforehand. Background `EraseRect(&gw->url_rect)` before both.
- **Hit-tested:** `point_in_rect(local, &gw->url_rect)` in the mouse-down handler, then `TEClick(local, shift, gw->url_te)`. Note: the point passed to `TEClick` is the raw local point, not a TE-rect-local point. TextEdit computes its own offsets internally.
- **Disposed:** `TEDispose` in `macos9_window_destroy` before `DisposeWindow`.

**Gaps:**
- **`url_rect` vs TE `viewRect` are offset by `MACOS9_URL_INSET` (2 pixels).** Click detection uses `url_rect`; TE's internal rect is smaller. Documented in §1.9.
- **No idle without port.** `macos9_windows_te_idle` saves and restores the port (good) and calls `TEIdle` for every TE whose flag is true. If a window's port has somehow diverged from its WindowRef's port (shouldn't ever happen, but there's no enforcement), `TEIdle` would render through the wrong port.

### 2.4 Chrome drawing composition

Summarizing how the chrome gets on screen:

1. `DrawControls(win)` in `macos9_handle_update`, paints all ControlRefs (buttons + scroll bars).
2. `draw_url_bar(gw)`, `EraseRect(&url_rect)`, `FrameRect(&url_rect)`, `TEUpdate(&url_rect, url_te)`.
3. `draw_status_bar(gw)`, `EraseRect(&status_rect)`, `FrameRect(&status_rect)`, `DrawText` of `gw->status`.
4. `browser_window_redraw(...)`, content via plotter table.

The order is deterministic (in update handler order: erase → DrawControls → draw_url_bar → browser_window_redraw → draw_status_bar → EndUpdate). The issue is that this is a single BeginUpdate/EndUpdate pair covering everything. Partial invalidates (e.g. `invalidate_url` alone) cause a re-paint of ONLY the URL rect, but then the update handler runs `DrawControls` over the whole window anyway, because `DrawControls` does not respect the update rgn. Effect: partial invalidates are honored only for the erase + draw_* subset; `DrawControls` always repaints all controls on any update. This is not a bug (Appearance Manager is fast, and `DrawControls` respects control visibility), but it means the `invalidate_*` granularity doesn't actually reduce chrome work.

---

## Section 3, Event dispatch flow

### 3.1 mouseDown

Entry: [main.c macos9_handle_mouse_down](../../browser/netsurf/frontends/macos9/main.c). Dispatched from `macos9_dispatch_event` only if the event passes the whitelist (fixes141).

Flow:

```
macos9_handle_mouse_down(event):
  FindWindow(event->where, &win) -> part code
  switch (part):
    inMenuBar:  MenuSelect -> macos9_handle_menu
    inDrag:     DragWindow(win, event->where, &screenBounds)
    inGoAway:   TrackGoAway -> macos9_window_destroy; last-window -> macos9_done
    inGrow:     GrowWindow -> SizeWindow -> macos9_window_resize
    inContent:  (see below)
    default:    (drop, logged)
```

`inContent` sub-dispatch:

```
  if (win != FrontWindow()): SelectWindow(win); break;
  gw = macos9_find_window(win)
  SetPortWindowPort(win)
  local = event->where; GlobalToLocal(&local)
  if (point_in_rect(local, &gw->url_rect)):
    te_activate_url; TEClick; break
  FindControl(local, win, &ctrl) -> cpart
  if (ctrl != NULL):
    te_deactivate_url (if active)
    if (ctrl in {vscroll, hscroll} && cpart != 0):
      handle_scrollbar_click(gw, ctrl, cpart, &local)
    elif (cpart != 0):
      TrackControl(ctrl, local, NULL) -> tc_result
      if (tc_result != 0):
        dispatch to back/forward/reload/home based on ctrl identity
  else:
    empty_content: te_deactivate_url (if active)
```

**State read by mouseDown path:**
- `gw->url_rect`, for URL hit test.
- `gw->url_field_active`, `gw->url_te`, for URL activation / deactivation decisions.
- `gw->vscroll`, `gw->hscroll`, `gw->back_btn`, `gw->forward_btn`, `gw->reload_btn`, `gw->home_btn`, for dispatch identity.

**State written:**
- `gw->url_field_active` (via te_activate / te_deactivate).
- Via dispatched handlers: `gw->scroll_x`, `scroll_y`, `content_width`, `content_height` (by `scrollbar_click` → `scroll_to` → `update_extents`), `gw->bw`'s navigation state (by button handlers).

**Invariants assumed:**
- `gw->window` is the WindowRef returned from `FindWindow`. Enforced by `macos9_find_window`.
- Port is the window's port before any `FindControl` / `TrackControl` call (`SetPortWindowPort(win)`).
- `FindControl` returns a ControlRef owned by `win`.
- `TrackControl`'s `NULL` action proc is safe with proc 384 (non-live CDEF, fixes159). With proc 386 or any UPP-dispatched action, unsafe (fixes147 UPP macro gotcha).

**Silent failures:**
- `FindWindow` returning a `win` that maps to no `gw` (no match in `window_list`) → `macos9_find_window` returns NULL, handler early-returns. Could happen for non-MacSurf windows (not currently, MacSurf doesn't open foreign windows). Silent drop.
- `inGrow` with `new_size == 0` → user cancelled the drag; silently ignored.
- `FindControl` returning `NULL` with `cpart == 0` → click landed on the window interior but not on any control, not in URL rect. Routed to "empty_content" branch.
- `FindControl` returning non-NULL with `cpart == 0` → logged as "control ctrl=%p cpart=0 (ignored)" then falls through the switch without dispatch. Happens during thumb drag if `FindControl` returns the control but `cpart` is 0 before TrackControl runs, debatable edge, likely won't happen with Mac control CDEFs.
- `TrackControl` returning 0 on a button (mouse released outside the button) → no navigation action fired. Correct, this is the "click-and-drag-off-to-cancel" Mac idiom.

### 3.2 keyDown / autoKey

Entry: [main.c macos9_handle_key_down](../../browser/netsurf/frontends/macos9/main.c).

Flow:

```
macos9_handle_key_down(event):
  ch = low byte of message; keycode = second-low byte
  if (modifiers & cmdKey):
    MenuKey(ch) -> handle_menu; return
  win = FrontWindow(); gw = find_window(win)
  if (gw == NULL): return
  if (gw->url_field_active && gw->url_te != NULL):
    if (ch == '\r' or 0x03): address_bar_submit; return
    if (ch == 0x1B):  /* Escape */
      get_url from bw; TESetText; te_deactivate_url; return
    SetPortWindowPort; TEKey(ch, gw->url_te); return
  if (handle_scroll_key(gw, keycode)): return
```

**State read:**
- `FrontWindow()` - implicit.
- `gw->url_field_active`, `gw->url_te`, `gw->bw`, `gw->content_width`, `gw->content_height`, `gw->content_rect`.

**State written:**
- `url_field_active` (via Escape path's `te_deactivate_url`).
- `scroll_x`, `scroll_y` (via `handle_scroll_key` → `scroll_to`).
- The URL field's TE text (on Escape, `TESetText` restores the bw's current URL; on `TEKey`, text insert/delete).

**State machine implied:**

Two modes: URL-focused (route to TE) and content-focused (route to scroll). The switch is `url_field_active`. Transitions:
- Focus URL: URL rect click (mouse down), window activate (auto-restore, fixes153), `macos9_window_create` default (fixes150).
- Unfocus URL: non-URL click (any control or empty content), Escape, window deactivate (TEDeactivate only, no flag change per current code, the flag stays true, but the window is no longer front so keystrokes go elsewhere).

**Bugs the gate produces:**
- **Escape-while-not-URL-focused has no escape meaning.** If the user is in scroll mode and hits Escape, the keycode doesn't map in `handle_scroll_key` and falls through silently. Minor.
- **Arrow keys while URL-focused don't scroll.** They go to TE as cursor-move within the URL text. By design.
- **No Tab to toggle focus.** Tab is not a scroll key; if URL-focused, Tab inserts a literal tab (TEKey handles it); if not URL-focused, Tab is dropped. No focus-cycle idiom.

### 3.3 updateEvt

Entry: [main.c macos9_handle_update](../../browser/netsurf/frontends/macos9/main.c).

Flow:

```
macos9_handle_update(event):
  if (macos9_quitting): return
  win from event->message; gw = find_window(win)
  if (gw == NULL || gw->window == NULL): return
  GetPort(&old_port); SetPortWindowPort(win)
  BeginUpdate(win)
  GetWindowPortBounds(win, &bounds); EraseRect(&bounds)
  DrawControls(win)
  draw_url_bar(gw)
  if (gw->bw != NULL):
    if (browser_window_redraw_ready(gw->bw)):
      reset counters
      clip = content_rect
      ctx = { interactive, background_images, plot=macos9_plotters }
      browser_window_redraw(gw->bw, content_rect.left - scroll_x, content_rect.top - scroll_y, &clip, &ctx)
      log counters
    else: log "bw not ready"
  draw_status_bar(gw)
  EndUpdate(win)
  SetPort(old_port)
```

**Clip stages:**
- `BeginUpdate` sets the visRgn to the update rgn (intersection of the invalidated area and the actual visible region).
- `EraseRect(&bounds)` erases the entire window content area, but clipped by the update rgn. So the erase is *not* actually whole-window, just the invalidated bits.
- `DrawControls` respects the visRgn. Same for `draw_url_bar`'s `TEUpdate`.
- `browser_window_redraw` is passed an explicit `clip` rect equal to `content_rect`. The plotter table's `plot_clip` is expected to honor it (see §5).
- `EndUpdate` restores the visRgn.

**Who sets the port:** the update handler, explicitly via `SetPortWindowPort(win)`. The port was saved and restored (`GetPort` / `SetPort`) so the caller's port is preserved.

**Scroll assumptions:**
- Redraw origin = `(content_rect.left - scroll_x, content_rect.top - scroll_y)`. This tells NetSurf core "draw document origin at this viewport position". If `scroll_y = 100`, content origin is at `content_rect.top - 100`, i.e. document rows 0..99 are above the viewport, row 100 is at the top of the viewport.
- Clip = `content_rect`. So NetSurf only draws into the content area, not into the chrome.
- `browser_window_redraw_ready(gw->bw)` gates this; when false, no redraw is performed. The erase + chrome draw still happen, so the user sees an empty content area with chrome.

### 3.4 activateEvt

Entry: [main.c macos9_handle_activate](../../browser/netsurf/frontends/macos9/main.c).

Flow (becoming_active == 1):

```
SetPortWindowPort(win)
if (gw->url_te != NULL):
  TEActivate(gw->url_te)
  url_field_active = true  (fixes153)
update_scrollbars(gw)
update_button_states(gw)
invalidate_all(gw)
```

Flow (becoming_active == 0):

```
SetPortWindowPort(win)
if (gw->url_te != NULL): TEDeactivate(gw->url_te)
for each control: HiliteControl(ctrl, 255); Draw1Control(ctrl)
invalidate_all(gw)
```

**State machine:**
- Activate: TE on, all controls to correct hilite, whole window invalidated.
- Deactivate: TE off, all controls grey, whole window invalidated.

**Interaction with `url_field_active`:**
- Activate forces `url_field_active = true` (if `url_te` exists). Clobbers prior state, even if the user had deactivated the URL field before switching apps, re-activation re-steals focus. Deliberate (fixes153 behavior).
- Deactivate does NOT clear `url_field_active`. Documented in §1.6 as a harmless drift.

### 3.5 growWindow / resize path

Flow (within the `inGrow` branch of `macos9_handle_mouse_down`):

```
GrowWindow(win, event->where, &limits) -> new_size
if (new_size != 0):
  SizeWindow(win, h, v, true)
  gw = macos9_find_window(win)
  macos9_window_resize(gw)
```

`macos9_window_resize` flow:

```
macos9_window_layout(gw)          # recomputes all rects + MoveControl/SizeControl on scroll bars
rewrite url_te destRect/viewRect; TECalText(gw->url_te)
macos9_window_update_scrollbars(gw)  # re-queries max_x/max_y, updates control extents
gw->needs_reformat = true
macos9_window_invalidate_all(gw)
```

On next `macos9_poll`:

```
macos9_windows_process_deferred()
  for each window with needs_reformat:
    reformat_in_progress = true
    browser_window_schedule_reformat(gw->bw)
    reformat_in_progress = false
```

**Order:**
1. Window resized (immediate).
2. Rects recomputed (immediate).
3. Scroll bars moved/resized (immediate).
4. URL TE rect rewritten (immediate).
5. Scroll bar extents re-queried (immediate).
6. `needs_reformat` set.
7. `invalidate_all` posted.
8. Return from `handle_mouse_down`.
9. Return from `dispatch_event`.
10. `poll` iterates: next pass calls `process_deferred` → `schedule_reformat`.
11. Next pass after that: update event processes `invalidate_all`.

**Is this order correct?** Yes, with one caveat: `invalidate_all` is posted before the reformat has actually run, so the first post-resize paint happens with the OLD content extents. On the next pass, `schedule_reformat` fires, which eventually re-lays out and posts its own `invalidate`. So two repaints happen: one stale, one fresh. Minor cosmetic issue, the user sees a flicker. Could be dodged by invalidating AFTER `schedule_reformat`, but the deferred-flag pattern makes that awkward.

**Missing step: buttons not moved.** See §1.2.

---

## Section 4, The deferred-reformat pattern

The pattern: set `gw->needs_reformat = true` in response to a resize event; drain once per event-loop pass in `macos9_windows_process_deferred`.

### 4.1 Why

Calling `browser_window_schedule_reformat` synchronously from `handle_mouse_down`'s `inGrow` branch causes re-entrant layout. NetSurf core's reformat pipeline may invoke `macos9_gw_event(GW_EVENT_UPDATE_EXTENT)`, which calls `update_extents`, which calls `update_scrollbars`, which calls `SetControlValue`, which in some configurations can trigger a CDEF redraw, which can post events that walk back through the dispatcher. The deferred pattern breaks the cycle at a known choke point.

### 4.2 Is the pattern load-bearing?

Yes. Without the deferral, we've had historical re-entrance crashes (referenced in CLAUDE.md as "infinite layout loops on resize"). The pattern is not papering over a deeper bug; it's a correct solution to a real cooperative-concurrency problem.

### 4.3 Could it be simpler?

Three alternatives considered:

1. **Single flag, no re-entrance guard.** The re-entrance guard `reformat_in_progress` protects against a direct call to `process_deferred` during the `schedule_reformat` call. Today no such direct call happens. But defense-in-depth: if a future hook added a poll inside a reformat callback, the guard would catch it. Keep.

2. **Event-loop-wide single-shot flag.** Instead of per-window flags, one global "reformat needed" flag. Simpler, but loses per-window scoping, if two windows resize in sequence, only one reformat fires. Reject.

3. **Microtask queue.** A general deferred-callback queue. `needs_reformat` becomes one specific callback. Overkill for two flags.

Verdict: the current pattern is correct and minimal. Don't change it.

### 4.4 Other deferred infrastructure

`macos9_quitting` is a different pattern: a global flag set *once* at shutdown start, checked by re-entrant callbacks to early-return. Not a queue, just a one-way gate.

`macos9_fetching`, `macos9_sched_active`, `macos9_stub_fetcher_active()`, `macos9_http_fetcher_active()` are the "fetcher is busy" signal that shortens the sleep tick. Not deferred actions, just state.

No other deferred queues exist in the frontend. Good.

---

## Section 5, Redraw pipeline

### 5.1 From "something changed" to pixels

1. **Caller identifies a dirty region.** One of:
   - `macos9_window_invalidate_all(gw)`, whole window content rgn.
   - `macos9_window_invalidate_content(gw)`, just `content_rect`.
   - `macos9_window_invalidate_url(gw)`, just `url_rect`.
   - `macos9_window_invalidate_status(gw)`, just `status_rect`.
   - `macos9_gw_invalidate(gw, const struct rect *rect)`, NetSurf core's request. Currently **ignores the rect** and calls `invalidate_content` (the whole content area). Intentional simplification (comment: `MS_LOG("gui inv"); macos9_window_invalidate_content(gw);`).

2. **Inval function.** `InvalWindowRect(gw->window, &rect)`. Posts to the Window Manager's update rgn.

3. **Event loop dispatches updateEvt.** Next `WaitNextEvent` pass picks it up.

4. **Update handler fires.** `macos9_handle_update`, see §3.3.

5. **Plotters fire.** `browser_window_redraw` walks the content tree and calls `macos9_plot_clip`, `macos9_plot_rectangle`, `macos9_plot_text`, etc. Each plotter sets QuickDraw state (colors, fonts, clip) and emits QuickDraw primitives (`PaintRect`, `DrawText`, etc.). See [plotters.c](../../browser/netsurf/frontends/macos9/plotters.c).

6. **EndUpdate.** Restores the visRgn.

### 5.2 Identify: who decides "what's dirty"?

- Chrome changes (URL text, status text, button enable) explicitly call the right `invalidate_*` function.
- Content changes (scroll, new content, reflow) invalidate the entire content area.
- Resize invalidates everything.
- NetSurf-core requests (`gui_window_table->invalidate`) are upgraded to "whole content".

Summary: **partial invalidate is preserved for chrome; content is always full-content-area invalidate.** This is a simplification that trades efficiency for correctness, fine on real hardware where the content area isn't that large, but it means any scroll forces a full content repaint.

### 5.3 Is partial repaint actually implemented?

Partially. The Window Manager handles rgn-based partial repaint at the Carbon level. Within our update handler:

- `DrawControls(win)` respects the visRgn (clipped by it).
- `EraseRect(&bounds)` respects the visRgn.
- `TEUpdate(&url_rect, url_te)` respects the visRgn.
- `draw_status_bar` respects the visRgn (`DrawText` is clipped).
- `browser_window_redraw` is passed `clip = content_rect`, the entire content area, NOT the visRgn-inferred dirty area. So **any content update redraws the whole content area**, regardless of what was invalidated.

This is safe but inefficient. A refactor that wanted partial content repaints would need to compute the intersection of `content_rect` and the visRgn bounds and pass that as `clip`.

### 5.4 Scroll offset vs scroll bars consistency

`browser_window_redraw` is called with an offset of `(content_rect.left - scroll_x, content_rect.top - scroll_y)`. This reads `gw->scroll_y` directly, NOT `GetControlValue(vscroll)`. So the authoritative scroll position for rendering is `gw->scroll_y`, not the control.

`macos9_window_update_scrollbars` and `macos9_window_scroll_to` both write `gw->scroll_y` and then `SetControlValue(vscroll, (short)scroll_y)`, in that order. The render path reads `gw->scroll_y`, which is the fresh value. The control is updated to match afterwards.

**This is the right ordering.** Render authority = model. Control = view. Control doesn't drive render directly.

The one place this can diverge is during `TrackControl` with the non-live CDEF: the thumb moves visually during drag (CDEF manipulates the control's value), but `gw->scroll_y` isn't written until `TrackControl` returns and `scroll_to` is called. The rendered content is still at the pre-drag position. This matches the fixes159 trade-off: thumb visual tracks live, content does not, content snaps on release.

### 5.5 Chrome redraw on content update

Every updateEvt runs `DrawControls + draw_url_bar + draw_status_bar`, even if only the content area was invalidated. This is wasted work, small in absolute terms, but wasted. A refactor that wanted to skip chrome redraw when only content was dirty would need to inspect the update rgn. Worth considering but low priority.

### 5.6 Redraw-ready gate

`browser_window_redraw_ready(gw->bw)` can return false if the content hasn't finished loading / parsing / laying out. In that case, chrome is drawn but content is skipped, and the content area ends up black or white-on-the-erased-background depending on Carbon theme. This is visible to the user as "blank content area while loading". Could be improved by painting a loading indicator in the gap, but not a bug.

---

## Section 6, Scroll math

### 6.1 Authorities

- **`gw->scroll_y`**, the model. What `browser_window_redraw` reads.
- **`GetControlValue(vscroll)`**, the view. What the thumb visually shows.

The contract: they are equal after any consistent state. Enforced by writing both in sequence in `macos9_window_scroll_to` and `macos9_window_update_scrollbars`.

### 6.2 Thumb drag (cpart = `kControlIndicatorPart`, 129)

With proc 384 (fixes159 non-live):
- `TrackControl(ctrl, pt, NULL)` runs a CDEF-internal drag loop. CDEF handles mouse tracking, updates control value on mouseup only.
- `TrackControl` returns.
- `handle_scrollbar_click` reads `GetControlValue(ctrl)` → that's the new thumb position.
- Calls `scroll_to(gw, scroll_x, new_y)` or `(new_x, scroll_y)` based on axis.
- `scroll_to` writes `gw->scroll_y`, writes `SetControlValue(vscroll, new_y)`, invalidates content. Next updateEvt renders the new position.

With proc 386 (what we had before fixes159):
- `TrackControl` fires an action proc every frame during drag. But we had no action proc (fixes147 removed the UPP). So thumb moved visually (CDEF's own callback to move the thumb) but no per-frame app notification. Same net behavior as proc 384 for thumb drag. The difference was that proc 386 was crashing inside the CDEF on real hardware, hence fixes159.

### 6.3 Page up/down (cpart = 22, 23)

```
page_v = view_h - 32; if (page_v < 16) page_v = 16
delta_y = ± page_v
scroll_by(gw, 0, delta_y)
```

Magic `32`: twice `MACOS9_KEY_SCROLL_STEP` pre-fixes160 (was 16), or `view_h - 2 * arrow_step` in spirit. It's "page is mostly-a-viewport, minus a small overlap so the user can see the top of the new page starts where the bottom of the old page ended". With `view_h = 254`, page step = 222.

The fixes156 log confirmed the observed step of 222 matches this math exactly.

### 6.4 Arrow clicks (cpart = 20, 21)

```
delta_y = ± MACOS9_KEY_SCROLL_STEP  /* 48 post-fixes160 */
scroll_by(gw, 0, delta_y)
```

### 6.5 Can the two authorities diverge?

Yes, during the brief interval between `GetControlValue` in `scroll_to`'s entry and `SetControlValue` in its exit, if during that interval any other code reads `GetControlValue`, it gets the old value while `gw->scroll_y` has the new one. No current code path does this (no read inside `scroll_to` after the write).

More meaningfully: during a `TrackControl` thumb drag, the CDEF writes the control value progressively, but `gw->scroll_y` isn't written until `scroll_to` is called after `TrackControl` returns. Anything that reads `GetControlValue` during the drag sees the in-progress value; anything reading `gw->scroll_y` sees the pre-drag value. No critical code path reads during the drag, but the divergence exists.

### 6.6 `max_y` staleness

Scenario:
1. Page loads to `content_height = 800`. `update_scrollbars` sets `SetControlMaximum(vscroll, 800 - view_h)`.
2. User scrolls to `scroll_y = 600`.
3. An image loads asynchronously, reflow happens, `content_height` changes to `900`. `GW_EVENT_UPDATE_EXTENT` fires. `update_extents` re-queries. `update_scrollbars` recomputes `max_y = 900 - view_h`, sets new control max.
4. Between steps 2 and 3, the CDEF thinks max is `800 - view_h`, which is less than the new max. If the user drags the thumb to the bottom during that interval, the control clamps drag to the old max.

Mitigation: `update_extents` fires frequently during load; the staleness window is short. `update_scrollbars` re-clamps `gw->scroll_y` to the new range. Fine in practice.

### 6.7 Summary of inconsistencies

Documented in §1.3, §1.7. The scroll model is model-authoritative (`gw->scroll_y`) with control as a synchronized view. No deep re-architecture needed.

---

## Section 7, Prior art: how real Carbon browsers structured this

The task is to examine specific architectural patterns in real Carbon / pre-Carbon apps that solve the problems MacSurf is wrestling with. Verified: iCab 3 is closed-source; IE5 Mac is closed; Classilla is the only source-available full-Carbon browser with the same scope as MacSurf. NetSurf's other frontends (RISC OS, AmigaOS, GTK, Cocoa) are the in-tree analogs. Apple's sample code (`AppearanceSample`, `ControlManager`, `ScrollingSample`) define the standard Carbon patterns.

This section does NOT paraphrase from memory, specific file paths are cited and what's cited is what was verified in prior rounds (see CLAUDE.md Prior Art section and the Classilla SourceForge reference therein).

### 7.1 Classilla (`xpfe/bootstrap` + widget, github.com/classilla/classilla)

Classilla is Mozilla-era XPCOM. Widget code lives under `widget/src/mac/` (Classic) or `widget/src/carbon/`. It runs a full Carbon event loop on OS 9 and is the closest architectural analog to MacSurf.

Classilla patterns relevant to MacSurf's window framework:

1. **Separation of "window" (nsWindow) and "content view" (nsView).** nsWindow handles Carbon events, owns the WindowRef and the chrome (toolbars are separate native controls OR drawn from nsView). nsView handles content rendering and knows nothing about Carbon event dispatch. MacSurf currently flattens these, `gui_window` holds both chrome widgets and content-layout state.

2. **Scroll bar as a Mozilla-owned Widget.** Classilla's `nsScrollbarFrame` contains an `nsNativeScrollbar` (a Carbon ControlRef wrapper) and a model object for the scroll position. The native scroll bar's value is written from the model; the control's action callback writes back to the model. One authority (the model), one synchronized view (the control). MacSurf's scroll path is structurally the same but spreads the authority across `gw->scroll_y` and `GetControlValue`.

3. **Sequenced layout pipeline.** Mozilla's `ProcessReflowCommands` processes a queue of reflow commands posted during event processing, draining the queue between events. MacSurf has a minimal version of this in `process_deferred`, but only handles one flag (`needs_reformat`), not a general command queue.

Cost of adopting more: large. Classilla's widget layer is several thousand lines. MacSurf doesn't need the full stack, but the **pattern** of "command queue drained between events" is worth adopting for anything beyond a single boolean flag.

### 7.2 NetSurf RISC OS frontend (`browser/netsurf/frontends/riscos/`)

The closest in-tree NetSurf frontend to MacSurf: cooperative multitasking on a non-POSIX OS, custom widget kit, its own event dispatcher.

Relevant patterns (from browsing the RISC OS frontend):

- **`gui_window` is a generic struct that the platform-specific window holds a pointer to.** Each platform defines its own struct and NetSurf core just passes it around. MacSurf follows this.
- **Scroll state is kept in the platform struct, not in NetSurf core.** Matches MacSurf.
- **The redraw callback is the platform's responsibility.** RISC OS's redraw handler is similarly structured to MacSurf's (receive a clip rect from the OS, pass it to `browser_window_redraw`).

RISC OS doesn't have a deferred-reformat pattern because its event model doesn't have the re-entrance risk MacSurf's mouse-down → GrowWindow → layout has. So the deferred pattern is MacSurf-specific and appropriate.

### 7.3 NetSurf Amiga frontend (`frontends/amiga/`)

Another cooperative-OS NetSurf frontend. Uses a message-port-based event loop (Intuition messages). Patterns:

- **`struct gui_window_2` holds the Amiga-specific state (Window pointer, Gadget pointers, scroll state).** Same pattern as MacSurf.
- **Gadgets (Amiga's "controls") are created at window creation and their bounds refreshed on window resize via `gui_window_redraw_window`'s resize path.** MacSurf recomputes scroll bars on resize but not toolbar buttons, the Amiga frontend is consistent across all gadgets. This is the pattern MacSurf should adopt.

### 7.4 Apple's Carbon sample code

`ScrollingSample` (canonical) demonstrates:

- **Scroll bars tracked via a single ControlRef, value authoritative.** The content's scroll offset is re-read from the ControlRef on every update. No parallel model. Opposite of MacSurf's model-authoritative approach.

Apple's convention is control-authoritative; MacSurf is model-authoritative. Both work; the important thing is to pick one and enforce it. MacSurf's current code is model-authoritative-with-careful-sync, which is fine, but it's worth making the model the ONLY source for any renderer code that reads scroll position. No code currently reads `GetControlValue` as the primary scroll position outside of `handle_scrollbar_click`'s thumb-drag path (which correctly forwards it to `scroll_to`).

### 7.5 Summary of prior art

The high-impact lesson: **make one authority for each piece of state, synchronize everything else to it, and name the authority explicitly.**

MacSurf's current violations:
- Scroll: model-authoritative, mostly consistent, one known divergence point (thumb drag).
- Focus: flag-authoritative (`url_field_active`), mostly consistent, one known divergence point (deactivate doesn't clear the flag).
- Control bounds: model-authoritative in layout code, control-authoritative for reads via `ContrlRect`; one known gap (toolbar buttons never updated).
- Chrome rects: model-authoritative (`gw->url_rect` etc.), one dead field (`toolbar_rect`).

All violations are minor and locally fixable. No rewrite needed; small refactors.

---

## Section 8, Architectural problems identified

Synthesizing §1-§7 into a ranked list of distinct architectural problems (not bugs).

### P1. Chrome bounds are partially computed-on-resize

**Symptoms:** Push buttons stay at creation bounds even after window resize. Latent: any resize-sensitive button layout change would expose.

**Root cause:** `macos9_window_layout` recomputes four rects and calls `MoveControl`/`SizeControl` on scroll bars only. Buttons are left out of the recomputation.

**Severity:** Latent. Current button layout is left-anchored with fixed positions, so no user-visible symptom today. Blocks: any future toolbar change that references window width.

### P2. Two authorities for scroll bar bounds

**Symptoms:** None. But the ControlRec's `contrlRect` and the implicit "inside `content_rect` + scrollbar column" are parallel authorities with no consistency check.

**Root cause:** Scroll bars' bounds live only on the ControlRef; MacSurf has no `gw->vscroll_rect`. Code that wants to reason about scroll bar geometry dereferences the ControlRef.

**Severity:** Low. Not causing bugs. Ergonomically awkward.

### P3. URL rect click-detection vs TextEdit viewRect are offset and unequal

**Symptoms:** None observed. Click at `url_rect.left + 1` is passed to `TEClick` even though TE's internal `viewRect` starts at `url_rect.left + MACOS9_URL_INSET`. TextEdit is forgiving and has not complained.

**Root cause:** `compute_url_te_rect` produces the inset rect for TE; hit detection uses the outer rect directly. No unification.

**Severity:** Low. Potential for a click geometry bug on pixel-edge clicks.

### P4. `url_field_active` flag can drift from `TEActivate` state

**Symptoms:** Documented in §1.6. Today: harmless. Deactivated window still has the flag set.

**Root cause:** `TEDeactivate` is called without clearing the flag; `TEActivate` is called with the flag already possibly true.

**Severity:** Low. One-click to set; one-deactivate-then-reactivate to reset.

### P5. Chrome is always fully redrawn even on content-only invalidate

**Symptoms:** Small performance overhead. Not visible.

**Root cause:** `DrawControls` unconditionally, `draw_url_bar` / `draw_status_bar` unconditionally, even if only `content_rect` was invalidated.

**Severity:** Low. Repaint cost on Appearance controls is small.

### P6. Partial content invalidate is downgraded to full-content invalidate

**Symptoms:** Scrolling (which could in principle invalidate just the scrolled-out strip) forces a full-content repaint every time.

**Root cause:** `macos9_gw_invalidate` ignores the rect and calls `invalidate_content` for the whole content area.

**Severity:** Medium. Noticeable on large content areas; not blocking.

### P7. `TrackControl`-return value is unused for scroll bars

**Symptoms:** Clicking the up-arrow then dragging into the down-arrow before releasing still scrolls up.

**Root cause:** `handle_scrollbar_click` uses the `cpart` from `FindControl`, not the value returned by `TrackControl` (which would be the final part code).

**Severity:** Low-to-zero. Edge case that almost no user hits.

### P8. Scroll bar value truncation at 32767

**Symptoms:** None today (no MacSurf page has `content_height > 32767`). Latent.

**Root cause:** `SetControlValue((short)gw->scroll_y)` truncates silently.

**Severity:** Latent. Blocks: large pages.

### P9. `content_height` staleness between layout passes

**Symptoms:** Scroll bars initially grey after page load; user must resize or wait for STOP_THROBBER to enable them.

**Root cause:** Early `UPDATE_EXTENT` captures pre-final content_height; no re-query until load completes.

**Severity:** Medium. Partially fixed by fixes160 (STOP_THROBBER re-query). Still present for async-reflow post-load (e.g. delayed image load).

### P10. No focus state machine; `url_field_active` is a bool

**Symptoms:** No Tab to cycle focus. Escape only works if URL-focused. No "future third widget takes focus" support.

**Root cause:** Focus is a one-bit boolean, not an enumeration.

**Severity:** Low. Forward-compatibility concern.

### P11. Deferred-reformat pattern is ad-hoc, not generalized

**Symptoms:** `needs_reformat` + `reformat_in_progress` solve one case; any future deferred action needs its own flag.

**Root cause:** No general command queue.

**Severity:** Low. Only one deferred action exists.

### P12. `toolbar_rect` is computed but unread

**Symptoms:** None. Dead field.

**Root cause:** Legacy. The two-row-toolbar reshuffle (fixes153) made the field redundant.

**Severity:** Zero. Cleanup only.

### P13. Chrome drawing concerns live across `window.c` and `main.c`

**Symptoms:** `draw_url_bar` and `draw_status_bar` live in `main.c`; `macos9_window_layout` lives in `window.c`. `point_in_rect` lives in `main.c` but tests `gw->url_rect` which is a window.c concern.

**Root cause:** `main.c` grew event-handler code that should be chrome-layer code.

**Severity:** Low. Organizational. Impedes navigation when reading the code.

### P14. Update handler has heavy instrumentation wired permanently

**Symptoms:** None. Small log-file overhead per redraw.

**Root cause:** fixes156, fixes157 added counter-reset + log. The log channel is gated by `MACSURF_DEBUG`, so release builds no-op. But the counter-reset code runs regardless of debug.

**Severity:** Low. The counter reset is inside the `#ifdef __MACOS9__` block but not inside any debug gate, so release builds still run the resets. Micro-optimization.

### P15. Initial-window URL-field behavior diverges from `File → New` (open question)

**Symptoms:** Per user reports (2026-04-18 survey §1, 2026-04-19 Track B.1): URL field on initial window behaves differently from File→New.

**Root cause:** Hypothesized as content-redraw overdrawing the URL rect. Not confirmed on hardware (survey §B1 probes not yet run).

**Severity:** Medium. User-facing bug. Needs a probe round, not a refactor.

---

## Section 9, Proposed unified window state model

The goal here is not a from-scratch rewrite. MacSurf's current structure is mostly correct; the refactor is about:

1. Single authority per piece of state, explicitly named.
2. All chrome bounds computed in one function, called in one way, on every relevant event.
3. Clear chrome / content separation in the redraw pipeline.
4. Explicit focus state machine.
5. Eliminating dead fields.

### 9.1 Revised `struct gui_window`

```c
struct gui_window {
    /* Identity */
    WindowRef window;          /* Carbon window handle, NULL after destroy */
    struct browser_window *bw; /* NetSurf core handle, may be NULL */

    /* Chrome widgets */
    struct {
        ControlRef back;
        ControlRef forward;
        ControlRef reload;
        ControlRef home;
    } buttons;
    ControlRef vscroll;
    ControlRef hscroll;
    TEHandle   url_te;

    /* Chrome geometry. All in window-local coordinates.
     * Invariant: recomputed atomically by compute_chrome_bounds();
     * all rects are consistent with gw->window's current port bounds. */
    struct {
        Rect url;           /* URL bar outer rect (frame, erase) */
        Rect url_te_view;   /* URL TE's viewRect (inset of url) */
        Rect content;       /* Content area */
        Rect status;        /* Status bar */
        /* scroll bar rects live on the ControlRefs (authoritative) */
    } rects;

    /* Content state. Model-authoritative. */
    int scroll_x, scroll_y;
    int content_width, content_height;

    /* Focus state machine. Replaces url_field_active bool. */
    enum { FOCUS_NONE, FOCUS_URL, FOCUS_CONTENT } focus;

    /* Command queue replacement for deferred-reformat flag.
     * Bitfield; drained once per poll pass. */
    unsigned needs_reformat : 1;
    unsigned reformat_in_progress : 1;  /* re-entrance guard */

    /* Status text */
    char status[128];

    /* List linkage */
    struct gui_window *next;
};
```

Changes from current:
- `buttons` grouped into a struct. Purely organizational.
- `rects` grouped into a struct. Removes the dead `toolbar_rect` field. Adds an explicit `url_te_view` rect so the offset-from-url is explicit.
- `url_field_active` bool replaced by `focus` enum.
- `needs_reformat` / `reformat_in_progress` made 1-bit bitfield for future extensibility (room for more deferred flags without ballooning struct size).

### 9.2 Chrome bounds computation

```c
/* Recompute all chrome bounds from the current window port bounds.
 * Called from window creation and from resize. No side effects outside
 * gw->rects and the scroll-bar / URL-TE control bounds. */
static void compute_chrome_bounds(struct gui_window *gw);
```

Implementation (one function, replaces scattered logic):

```c
static void
compute_chrome_bounds(struct gui_window *gw)
{
    Rect content;
    short win_w, win_h;
    short hscroll_top;
    short content_bottom;
    short x;
    Rect button_rect;

    GetWindowBounds(gw->window, kWindowContentRgn, &content);
    win_w = content.right - content.left;
    win_h = content.bottom - content.top;

    /* URL bar (row 2 of toolbar, full width) */
    SetRect(&gw->rects.url,
        MACOS9_BTN_GAP, MACOS9_URL_ROW_Y,
        win_w - MACOS9_BTN_GAP,
        MACOS9_URL_ROW_Y + MACOS9_URL_ROW_HEIGHT);

    gw->rects.url_te_view.left   = gw->rects.url.left   + MACOS9_URL_INSET;
    gw->rects.url_te_view.top    = gw->rects.url.top    + MACOS9_URL_INSET;
    gw->rects.url_te_view.right  = gw->rects.url.right  - MACOS9_URL_INSET;
    gw->rects.url_te_view.bottom = gw->rects.url.bottom - MACOS9_URL_INSET;

    /* Content and status */
    hscroll_top    = win_h - MACOS9_SCROLLBAR_WIDTH;
    content_bottom = hscroll_top - MACOS9_STATUS_HEIGHT;
    SetRect(&gw->rects.content,
        0, MACOS9_TOOLBAR_HEIGHT,
        win_w - MACOS9_SCROLLBAR_WIDTH, content_bottom);
    SetRect(&gw->rects.status,
        0, content_bottom,
        win_w - MACOS9_SCROLLBAR_WIDTH, hscroll_top);

    /* Toolbar buttons — moved in resize (new behavior) */
    x = MACOS9_BTN_GAP;
    SetRect(&button_rect, x, MACOS9_BTN_Y,
        x + MACOS9_BTN_WIDTH, MACOS9_BTN_Y + MACOS9_BTN_HEIGHT);
    MoveControl(gw->buttons.back, button_rect.left, button_rect.top);
    SizeControl(gw->buttons.back, MACOS9_BTN_WIDTH, MACOS9_BTN_HEIGHT);
    x += MACOS9_BTN_WIDTH + MACOS9_BTN_GAP;
    MoveControl(gw->buttons.forward, x, MACOS9_BTN_Y);
    SizeControl(gw->buttons.forward, MACOS9_BTN_WIDTH, MACOS9_BTN_HEIGHT);
    /* ... reload, home */

    /* Scroll bars */
    MoveControl(gw->vscroll,
        win_w - MACOS9_SCROLLBAR_WIDTH,
        MACOS9_TOOLBAR_HEIGHT - 1);
    SizeControl(gw->vscroll,
        MACOS9_SCROLLBAR_WIDTH + 1,
        content_bottom - MACOS9_TOOLBAR_HEIGHT + 2);
    MoveControl(gw->hscroll, -1, hscroll_top);
    SizeControl(gw->hscroll,
        win_w - MACOS9_SCROLLBAR_WIDTH + 2,
        MACOS9_SCROLLBAR_WIDTH + 1);

    /* URL TE internal rect */
    if (gw->url_te != NULL) {
        struct TERec **th = (struct TERec **)gw->url_te;
        HLock((Handle)gw->url_te);
        (**th).destRect = gw->rects.url_te_view;
        (**th).viewRect = gw->rects.url_te_view;
        HUnlock((Handle)gw->url_te);
        TECalText(gw->url_te);
    }
}
```

### 9.3 Chrome sync

```c
/* Push the current model state (scroll, history, content extents) to
 * the live controls. Replaces the scattered update_scrollbars +
 * update_button_states pair. Called from:
 *   - macos9_window_create (after chrome created)
 *   - macos9_handle_activate (on activate)
 *   - macos9_gw_event (on UPDATE_EXTENT, NEW_CONTENT, STOP_THROBBER,
 *                      and button-state-changing events)
 *   - macos9_window_scroll_to (after model mutation) */
static void sync_chrome(struct gui_window *gw);
```

Implementation sketch: composition of the current `update_scrollbars` and `update_button_states`, without the duplication.

### 9.4 Redraw pipeline separation

Proposed:

```c
static void paint_chrome(struct gui_window *gw);
static void paint_content(struct gui_window *gw);

static void
macos9_handle_update(const EventRecord *event)
{
    ...
    BeginUpdate(win);
    /* Chrome: always. DrawControls respects visRgn, so no-op on
     * content-only invalidate in practice. */
    paint_chrome(gw);
    /* Content: only if visRgn intersects content_rect. */
    if (rgn_intersects_rect(get_vis_rgn(), &gw->rects.content)) {
        paint_content(gw);
    }
    EndUpdate(win);
    ...
}
```

Separation cost: low. Benefit: future partial-content-invalidate work becomes a one-place change.

### 9.5 Focus state machine

States: `FOCUS_NONE`, `FOCUS_URL`, `FOCUS_CONTENT`.

Transitions:

| From → To | Trigger |
|---|---|
| `*` → `FOCUS_URL` | Click in URL rect; window activate if `url_te != NULL`; window create |
| `FOCUS_URL` → `FOCUS_CONTENT` | Click in content area (empty or control) |
| `FOCUS_URL` → `FOCUS_NONE` | Escape key; window deactivate |
| `FOCUS_CONTENT` → `FOCUS_URL` | Tab? (future); click in URL rect |

Entry action (set_focus_url): `TEActivate`; `invalidate_url`.
Exit action (clear_focus_url): `TEDeactivate`; `invalidate_url`.

```c
static void set_focus(struct gui_window *gw, int new_focus)
{
    if (gw->focus == new_focus) return;
    switch (gw->focus) {
    case FOCUS_URL:
        if (gw->url_te != NULL) TEDeactivate(gw->url_te);
        macos9_window_invalidate_url(gw);
        break;
    default: break;
    }
    gw->focus = new_focus;
    switch (new_focus) {
    case FOCUS_URL:
        if (gw->url_te != NULL) TEActivate(gw->url_te);
        macos9_window_invalidate_url(gw);
        break;
    default: break;
    }
}
```

Replaces `url_field_active` + scattered TEActivate/TEDeactivate calls.

### 9.6 Scroll model

Keep current model-authoritative approach. Make it explicit:

```c
/* Scroll is model-authoritative. gw->scroll_x, scroll_y are the
 * source of truth. The ControlRef is synchronized to the model by
 * sync_scroll_controls(). No caller reads GetControlValue() outside
 * of the thumb-drag-return path, which immediately forwards it to
 * scroll_to (which writes the model and then syncs the control). */
static void sync_scroll_controls(struct gui_window *gw);
```

Add the 32000-pixel clamp to `scroll_to` (to mirror the clamp in `update_scrollbars`):

```c
if (new_y > 32000) new_y = 32000;  /* SInt16 ceiling */
```

---

## Section 10, Refactor plan

Six rounds. Each leaves MacSurf buildable and runnable. Each ships as a monotonically-numbered fix zip via the standard delivery pipeline.

### fixes161, This document (no code)

This document lives at `docs/research/window-architecture-2026-04-22.md`. Shipped alone as a research commit. No source changes. CLAUDE.md updated with one-line pointer under "Docs".

**Acceptance:** File exists, committed. CLAUDE.md updated. No behavior change.

### fixes162, Unified chrome bounds computation

**Scope:** P1, P13 (partial), P12.

**Files:** `window.c`, `macos9.h`, `main.c` (no change beyond header include if needed).

**Change:**
1. Introduce `compute_chrome_bounds(gui_window *gw)` in window.c (see §9.2).
2. Move all `MoveControl` / `SizeControl` / TE rect rewrites into this function.
3. Remove scattered rect-computing logic from `macos9_window_layout` and `macos9_window_resize`.
4. Delete `gw->toolbar_rect` (P12). Purge any readers (grep first, should return zero hits).
5. Add `gw->rects.url_te_view` as an explicit field (or compute locally; decide on encapsulation).
6. Move `point_in_rect` from main.c into window.c as a local helper, OR make it inline in the URL-click detection (P13 partial).

**Risk:**
- Button bounds now recomputed on resize. If any button is positioned in a way that depends on creation-time bounds NOT being overwritten, that assumption breaks. Very unlikely.
- Any code that reads `gw->toolbar_rect` will fail to compile. Grep confirms zero such readers today.

**Acceptance:**
- Window creation: layout unchanged.
- Resize: buttons stay in position (same left-anchored positions, same size). No regression.
- Resize stress: grab grow box, drag to small size and back. No crash, no layout drift.

**Dependencies:** None.

### fixes163, Focus state machine

**Scope:** P10, P4.

**Files:** `window.c`, `main.c`, `macos9.h`.

**Change:**
1. Replace `bool url_field_active` with `enum { FOCUS_NONE, FOCUS_URL, FOCUS_CONTENT } focus` in `struct gui_window`.
2. Introduce `set_focus(gw, new_focus)` as the only mutator.
3. All current writes to `url_field_active` routed through `set_focus`.
4. `macos9_handle_key_down`'s URL-path gate becomes `gw->focus == FOCUS_URL`.
5. `macos9_windows_te_idle` gate becomes `gw->focus == FOCUS_URL`.
6. `macos9_handle_activate` on deactivate now clears focus to `FOCUS_NONE` (not leaving stale).

**Risk:** Medium. The focus path is touched by several handlers. Easy to miss a call site.

**Acceptance:**
- Typing in URL bar works same as today.
- Window deactivate/reactivate restores URL focus (same as fixes153).
- Click in content area clears URL focus (same as today).
- Escape key clears focus (slightly different from today, today Escape only deactivates, leaving the flag stale).

**Dependencies:** fixes162 (not strict, but cleaner on the refactored base).

### fixes164, Scroll authority clarification + 32000-pixel clamp

**Scope:** P8 (clamp), clarifies §1.3, §1.7.

**Files:** `window.c`.

**Change:**
1. Add `if (new_y > 32000) new_y = 32000;` (and x) to `macos9_window_scroll_to`.
2. Add a comment to `struct gui_window scroll_x, scroll_y` explicitly naming them as the authoritative scroll state, with `GetControlValue(vscroll)` as the synchronized view.
3. Rename `update_scrollbars` → `sync_scroll_controls` to match the naming convention from §9.3 (optional, low-value).

**Risk:** Very low. The clamp is an added-only check.

**Acceptance:**
- Large synthetic page (`content_height = 40000`) doesn't crash or garbage-display.
- Current MacTrove rendering unchanged.

**Dependencies:** None (can ship independent of 162/163).

### fixes165, Chrome / content redraw separation

**Scope:** P5, P6.

**Files:** `main.c`, `window.c`, `macos9.h`.

**Change:**
1. Split `macos9_handle_update` into `paint_chrome(gw)` + `paint_content(gw)` internal helpers.
2. `paint_content` takes an explicit clip rect (passed from the update handler based on visRgn ∩ content_rect).
3. `macos9_gw_invalidate` respects the passed rect (not always full content).
4. `browser_window_redraw`'s `clip` argument = the actual clip (possibly smaller than `content_rect`).

**Risk:** Medium-high. Clip math is easy to get wrong. Test with small dirty rects (invalidate a 20×20 corner) to confirm nothing outside it gets overwritten.

**Acceptance:**
- Scrolling still works.
- Chrome rendered on every update, content rendered only when content area intersects dirty rect.
- Observable: log output shows `paint_content` not called when only `invalidate_url` was posted.

**Dependencies:** fixes162 (rects need to be computed centrally).

### fixes166, Generalize deferred-action pattern

**Scope:** P11.

**Files:** `window.c`, `macos9.h`.

**Change:**
1. Replace `needs_reformat` / `reformat_in_progress` pair with a per-window `deferred_actions` bitfield + `actions_in_progress` bitfield.
2. Currently one bit: `DEFERRED_REFORMAT`. Future bits can add without struct churn.
3. Drain function iterates bits, dispatches each.

**Risk:** Low. Pure refactor, no behavior change.

**Acceptance:** Resize still works. Deferred drain still fires once per poll pass.

**Dependencies:** None (can ship with any earlier round).

### Note, open issues NOT addressed by this plan

These are not refactor-shaped. They need their own investigations:

- **P15 (initial-window URL-field)**, needs a probe round per 2026-04-18 survey §1 and 2026-04-19 survey B1. Not addressable by refactor.
- **Wheel crash**, hardware-specific, needs MacsBug trace. Not addressable from Linux.
- **Scroll bar live-tracking**, proc 386 crashes on hardware. fixes159 chose proc 384; live-tracking remains deferred until MacsBug evidence is available. Not addressable by refactor.

These issues are documented in CLAUDE.md's "Next work queue" section and should stay there, not in this refactor plan.

---

## Appendix, cross-references

- [CLAUDE.md](../../CLAUDE.md), "Known Gotchas" covers the TE / `kWindowStandardHandlerAttribute` / wheel / scroll-bar-UPP / proc-386 hardware issues this document references.
- [docs/research/state-survey-2026-04-18.md](state-survey-2026-04-18.md), URL-field bug hypothesis (§1), CSS_NOMEM resolution (§2, now closed).
- [docs/research/state-survey-2026-04-19.md](state-survey-2026-04-19.md), CSS3 strategy decision (§A7), Track B chrome verification plan.
- [browser/netsurf/frontends/macos9/window.c](../../browser/netsurf/frontends/macos9/window.c), all state mutators for `gui_window`.
- [browser/netsurf/frontends/macos9/main.c](../../browser/netsurf/frontends/macos9/main.c), all event dispatchers.
- [browser/netsurf/frontends/macos9/plotters.c](../../browser/netsurf/frontends/macos9/plotters.c), redraw primitives; confirms no plotter reads `gw` directly (all state passes through NetSurf core).
- [browser/netsurf/frontends/macos9/macsurf_debug_log.c](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c), diagnostic channel used throughout for instrumentation; referenced here only as infrastructure.
- [browser/netsurf/frontends/macos9/macos9_wheel.c](../../browser/netsurf/frontends/macos9/macos9_wheel.c), intentional no-op; confirms the wheel path is CarbonLib-unavailable and documents why not to re-attempt.

The refactor plan (fixes162 through fixes166) is sized so each round fits in one typical session's scope (~1-4 hours), ships one self-contained change, and leaves MacSurf functionally identical or strictly improved at every stage. No round depends on a later round to restore correctness.
