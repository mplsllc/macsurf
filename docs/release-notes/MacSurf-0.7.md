# MacSurf 0.7 — Cleanup

**Released:** 2026-05-26
**Codename:** Cleanup
**Engine HEAD:** fixes271b
**Verified on:** Power Macintosh G3 iMac, Mac OS 9.1

---

## The headline

**Twelve issues closed in a single day.** v0.7 is the "resolve all known issues" sprint — a focused correctness + cleanup round on top of v0.6.2's speed work. Nine commits (fixes264 → fixes271b) cleared the entire open-issues backlog on `mplsllc/macsurf`. Author CSS now reaches more paint paths, image-heavy navigation stays memory-clean, CSS Grid honors author alignment intent, and a long-standing struct-naming workaround got resolved at the root.

Two things stand out from the round:
1. **Element-scoped `var()` resolution.** Author CSS like `.header { background-image: var(--tile) }` now resolves correctly when `--tile` is defined inline on a different element's `style="..."` attribute. Mactrove's `--header-tile` random-header pattern just works.
2. **CSS Grid V2 alignment** with zero libcss surgery. The previous attempt (fixes159) had failed in nine rounds trying to add a new vendor property; turned out libcss already had public accessors for `justify-content`, `align-content`, `align-items`, `align-self` — just had to consume them.

---

## What landed (by area)

### CSS correctness

- **fixes264 — `var()` resolves element's own inline `--custom-prop`.** Threads the cascading element's `inline_style` sheet through `lookup_var()` as highest-priority source. Works for the same-element case (element defines and uses the var).
- **fixes267 — Doc-global inline-extras custom-property table.** Aggregates `custom_properties` from every `style="..."` attribute parsed by `nscss_create_inline_style` into a doc-global table consulted by `lookup_var()`. Covers the cross-element case (parent defines, child uses). Cleared per `html_create` so per-page state doesn't bleed. Pragmatic V1 — last-writer-wins for name collisions, which covers mactrove's single-`--header-tile`-element pattern.
- **fixes266 — `background-image: linear-gradient(...)` rewrite.** New cssh_css preprocessor pass renames `background-image:` to `-macsurf-gradient:` when the value's first non-whitespace token is `linear-gradient(` or `radial-gradient(`. Author CSS using standard CSS3 syntax now reaches the existing vendor gradient paint path. Stacked layered backgrounds still drop on the trailing comma (no regression). Wired into both external sheets and inline-style attributes.
- **fixes270 — CSS Grid V2 alignment.** `layout_grid` consumes `css_computed_justify_content` (inline-axis track distribution: flex-end, center, space-between, space-around, space-evenly) and `css_computed_align_content` (block-axis distribution when there's a definite container height). Discovery that closed the prior 9-round fixes159 stall: libcss already exposed these accessors — no new vendor property needed. fixes270a preserves the CSS-declared container height through the redistribution so the box paints at full declared size instead of shrinking back to natural content height. `align-items` / `align-self` had already shipped at fixes178d; `justify-items` / `justify-self` deferred (libcss does not expose those accessors).

### Image / memory hygiene

- **fixes265 — Direct image URL navigation scale-fit-to-cap.** Typing a large image URL (e.g. `https://example.com/photo.jpg`) in the address bar pre-fix rendered blank because browser_window passed natural dimensions (4000×3000 → 48 MB RGBA, exceeds the 16 MB per-image ceiling). Pre-clamps `dst_w`/`dst_h` proportionally in `macos9_qt_image_redraw` before the cap-check, preserving aspect ratio. Integer-only math (CW8 PPC `long long` is unsafe per the well-documented gotcha).
- **fixes268 #10 — Decoded-image purge on top-level nav.** New `macos9_purge_decoded_images()` evicts the QT-deferred LRU and zeroes `macsurf__decoded_img_bytes_current`. Called from `browser_window_content_ready` after `hlcache_handle_release` of the old current_content; `llcache_clean(true)` purges the low-level cache alongside. Heavy-page → heavy-page navigation now starts with fresh budget headroom. Verified on hardware as `img purge: evicted=1769472 remaining=0` on a real nav.
- **fixes269 #8 — Above-fold lazy decode.** `macos9_qt_image_redraw` now early-returns when the image's destination rect lies entirely outside the redraw clip. NetSurf core's box-tree walker prunes most below-fold boxes but occasionally invokes redraw on parents whose bbox intersects the clip while specific children don't. Below-fold images stay undecoded until the user scrolls into them; scroll fires a new redraw with a different clip that triggers the decode.

### Diagnostic visibility

- **fixes268 #9 — Total CSS budget by importance.** New 384 KB cumulative cap on CSS bytes per page in `nscss_process_data`. Sheets arriving after the cap is hit short-circuit immediately (document order wins — site's main stylesheet processes fully, vendor tail sheets get truncated). `macsurf__site_css_total_bytes` reset per page in `html_create`.
- **fixes268 #11 — Heavy mode + `blocker=` field.** SITE diagnostic line now emits `heavy=N blocker=NAME` (none / img_budget / css_budget / fetch_slots / fonts) and `css_total=N/cap`. `heavy=1` latches when any skip counter is non-zero; blocker name picked by priority order. Visible at every `html_reformat` completion.
- **fixes268a — SITE line unsilencer.** fixes253 had silenced `SITE url=...` lines by default as a log-noise reduction. v0.7 requires the SITE line for `css_total=` and `blocker=` verification per the issue acceptance criteria, so SITE is back to firing per reformat. Low volume — survivable.

### Multicol

- **fixes268 #17 — Multicol balancing via bisection.** `layout_multicol_inner`'s per-segment `target_height` now goes through a new `multicol_compute_target_height()` helper:
  - **`column-fill: auto`** uses the viewport height as target (was identical to balance pre-fix; `auto` was unimplemented).
  - **`column-fill: balance`** starts with `max(tallest, ceil(total/count))` heuristic, then bisects downward (6 iterations, 8px convergence) to the smallest target that still fits in `count` columns. Tighter, more visually balanced output for non-uniform item heights.

### Build cleanup

- **fixes271b — `box_multicol_data` rename + Linux/Mac box.h alignment.** The `_ns` suffix on `struct box_multicol_seg_ns` and `struct box_multicol_dat_ns` was a fixes199h workaround for a CW8 redefinition error. Root cause turned out to be that the Mac's `box.h` had been carrying these struct definitions plus a `multicol_data` field on `struct box` since fixes199h, while the Linux `box.h` had neither. Aligned: added the structs + field to Linux's `box.h`, removed the duplicates from `layout_internal.h`, restored standard names (`box_multicol_segment`, `box_multicol_data`) in `layout.c`. Source trees now match across platforms; single source of truth in `box.h`.

---

## Issues closed (12)

All on [mplsllc/macsurf](https://github.com/mplsllc/macsurf/issues?q=is%3Aissue+is%3Aclosed):

| # | Title | Resolution |
| --- | --- | --- |
| #2 | Image memory pressure on modern image-heavy pages | fixes268 #10 (purge on nav) + existing fixes161b/162/259/265 deferred-decode |
| #3 | `%u` specifier in `macsurf_debug_log_writef` calls | Audit found no remaining hits; already resolved |
| #4 | CSS Grid V2 alignment | fixes270 / fixes270a |
| #5 | Wheel-mouse scroll can crash | Not reproducible on user's hardware; fixes140/141 defensive disable remains effective |
| #8 | Lazy image decode + above-the-fold bias | fixes269 |
| #9 | Total CSS budget by importance | fixes268 |
| #10 | Purge memory cache + decoded-image budget on top-level navigation | fixes268 |
| #11 | Heavy-site mode + `blocker=` field in SITE diagnostic line | fixes268 |
| #12 | fixes161 memory sweep umbrella | All constituent issues shipped |
| #17 | Multicol V2 — Balancing | fixes268 |
| #18 | Header Audit & Build Cleanup (Struct Renaming) | fixes271b |
| #22 | var() referencing parent's inline `--custom-prop` drops | fixes267 |

Plus #23 (background-image gradient drops) and #24 (direct image URL blank) filed and closed for the early-session work (fixes266, fixes265).

---

## Verified on hardware

- **test.html** — fixes264/266/267 probes: purple cross-element `var()` block + four gradient blocks (vertical 2-stop, horizontal 2-stop, vertical 3-stop, radial). All paint correctly post-fixes267.
- **test2.html** — fixes268 probes: M1 multicol balance with mixed heights + M2 column-fill: auto in 200px container. Plus fixes267/266 regression checks.
- **test3.html** — fixes270 probes GA1–GA7: justify-content (center, flex-end, space-between, space-around, space-evenly), align-content: center (200px container, fixes270a confirmation), align-items: flex-end regression check. All seven probes render correctly.
- **MacSurf Debug.log** confirms fixes268 SITE line: `heavy=0 blocker=none ... css_total=N/393216 ...` per reformat, `img purge: evicted=N remaining=0` on real navigation events.

---

## Known limitations carried forward

- **`justify-items` / `justify-self`** (cell horizontal alignment on grids) — libcss in this vintage does not expose `css_computed_justify_items` / `css_computed_justify_self`. Either libcss surgery (the trap zone — see project memory) or a `text-align` workaround would close the gap. Lower priority since container-level distribution (V1 shipped) covers most layouts.
- **Stacked `background-image: linear-gradient(...), linear-gradient(...)`** — single-gradient case works; multi-layer parser fails on the trailing comma. Same as pre-fix behaviour, no regression.
- **TLS fingerprint blocks** (Google / Facebook) — carried from v0.6.x; needs ALPN extension support in macTLS or a TLS 1.3 swap. The persistent dead-host list makes them fast-fail.
- **`gap: A B` two-value form** — single value works (97% of cases), two-value collapses to `column-gap=B`. Full-fidelity `row-gap` storage requires a new bit slot in `css_computed_style_i.bits[]`.
- **JPEG photo plot still slow when scrolling** — pre-scale at decode time is partially shipped (fixes162 + fixes259), but real-content scroll jank persists. Per-tick re-decode vs cached display-size bitmap is the next look.

---

## Build

`MacSurf.mcp` did not need to add any new `.c` files for v0.7 — every fix touched existing source files. Three header-level cleanups in fixes271b across `box.h`, `layout_internal.h`, `layout.c`. CW8 partition unchanged (16 MB preferred / 8 MB minimum).

---

## SheepShaver smoke

OS 9.0.4 in SheepShaver still good for Carbon init / UI / rendering smoke. Networking remains the SheepShaver limitation; not a substitute for hardware fetcher testing.

---

## Download

- **[MacSurf.sit](https://github.com/mplsllc/macsurf/releases/download/v0.7/MacSurf.sit)** — ready-to-run StuffIt archive. Expand on Mac OS 9.1+ with CarbonLib 1.5+ and launch.
- Source: this repository at tag `v0.7`.
