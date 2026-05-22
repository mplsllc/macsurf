# MacSurf CSS Implementation Plan

Companion to `CSS_SUPPORT_MATRIX.md`. The matrix says **what** every
CSS feature's status is. This document says **how and in what order**
to close the gaps.

Rules carried in from the standards directive:

- No feature lands without a probe under `tests/css/`.
- No round adds a new libcss property if a preprocessor /
  lowering / existing-storage path exists (see "Trap zones" below).
- Every shipped round updates `CSS_SUPPORT_MATRIX.md` row(s).
- "Done" means FULL, PARTIAL-with-documented-limits,
  QUICKDRAW_FALLBACK-with-documented-substitute, or
  INTENTIONALLY_UNSUPPORTED-with-reason. No "unknown" bucket.

## Trap zones (don't repeat these)

1. **New libcss `CSS_PROP_*` mid-insertion** — shifts every later
   property's enum index, dispatch tables become wrong, silent
   crashes (fixes178 root cause). Always append at end.
2. **Inline arrays / sub-int32 fields in `css_computed_style_i`** —
   memcmp interning corrupts when padding bytes diverge between
   cascade paths (fixes117, fixes151b). Use outer-struct + arena
   compare, or use a 4-byte-aligned field.
3. **Property-name aliasing for `transform` / etc.** — fixes141 hung
   pre-reformat. Use the `cssh_css.c` preprocessor (fixes175) for
   standard → vendor bridges.
4. **Reader-mode / domain-specific bypasses** — explicitly off-table.
5. **One symptom → one site → endless rounds** — every round must
   move the matrix forward, not just one site.

## Track order

Tracks roughly correspond to fixes200..fixes208. Each is a sprint,
not a single fix.

**Numbering note:** the prior in-session doc commit landed with the
label "fixes200" and the layout.c ship landed with the label
"fixes202" — both wrong; monotonic counter from fixes176 means those
were actually **fixes177 (docs)** and **fixes177 (tar = layout.c)**.
The tar shipped to the Mac is `fixes177.tar`. Track labels below
reflect the corrected counter; the commits in git stay as-is.

| Track | Round | Focus | Risk |
|---|---|---|---|
| A | fixes177 (docs) | Standards map (this file + matrix + audit script + tests README) | none |
| B | — | Flex intrinsic sizing — already shipped at fixes176 | low |
| C | fixes177 (tar) | Fast-lane parsed-not-consumed pack | low |
| D | fixes178 | Grid completion (areas, alignment, grid-row) | medium |
| E | fixes179 | Multi-column layout | medium |
| F | fixes180 | Selectors + at-rules pack | medium |
| G | fixes181 | Tables completion | medium |
| H | fixes182 | Paint / effects completion | medium |
| I | fixes183 | Transitions / animations feasibility | high |

---

## Track A — fixes200: standards map

**Shipped.** This file + `CSS_SUPPORT_MATRIX.md` + `tools/css_audit.sh`
+ `tests/css/README.md`. Baseline inventory under `tools/audit/`.

Acceptance:

- `tools/css_audit.sh` runs clean and writes `tools/audit/SUMMARY.txt`.
- Every accessor in `tools/audit/parsed_not_consumed.txt` has a row in
  the matrix.

---

## Track B — fixes201: flex intrinsic sizing

**Status:** fixes176 (last commit on master) already shipped flex
intrinsic main-size resolution. Apple's global nav and similar
auto-basis-on-indefinite-width flex containers now resolve. Probe:
`tests/css/flex_intrinsic.html` exists with probes I1-I4.

**Track B is therefore a follow-up sprint, not a new feature:**

1. Extend `flex_item_intrinsic_main_size` to the column-direction
   main axis. Currently returns 0 when `b->height` is AUTO and the
   item has no concrete height. Path: run a constrained
   `layout_block_context` against a sanitised available-width
   pulled from the parent walk, take resulting `b->height` as the
   intrinsic main size for column flex.
2. `min-width: auto` content-based shrink floor on flex items. Per
   spec, `min-width: auto` on a non-overflow flex item resolves to
   min-content. Currently treats `auto` as 0.
3. Parent walk depth tuning. Current cap is 16. Verify against deep
   real-world flex nesting (TailwindCSS-style class soup).
4. New probe cards I5 (column flex intrinsic) and I6 (min-width: auto)
   appended to `flex_intrinsic.html`.

Acceptance:

- Apple global nav unchanged (regression guard).
- A `display: flex; flex-direction: column;` container without
  explicit `height` no longer collapses text-only items to height 0.

---

## Track C — fixes202: fast-lane parsed-not-consumed pack

**No new libcss properties.** Every item below has a parser and a
public accessor; only the consumer is missing.

Group into one round because each consumer is small.

| Property | Site | Implementation |
|---|---|---|
| `pointer-events: none` | hit-test in `box_at_point` / `redraw.c` | skip box in hit test when accessor returns `CSS_POINTER_EVENTS_NONE` |
| `text-decoration-color` | `redraw.c` `text_redraw_decoration` | swap the colour passed to the underline/strike PenColor |
| `list-style-type` extensions (`lower-alpha`, `upper-alpha`, `lower-roman`, `upper-roman`, `decimal-leading-zero`) | `box_construct_marker` marker formatter | extend the `switch (list_style_type)` to format the integer counter accordingly. Pure C. |
| `word-break: break-all` / `keep-all` | inline layout break decision (`layout_line` / inline_break.c) | treat `break-all` as "break-anywhere"; `keep-all` ⇒ never break inside CJK runs (CJK detection: byte range U+4E00.. on the MacRoman side will round-trip to '?' so for OS 9 reality the behaviour is best-effort) |
| `overflow-wrap` / legacy `word-wrap` | same site | treat `break-word` as "break at any char if no break opportunity in line" |
| `orphans` / `widows` / `page-break-*` / `break-*` | none | declared no-op; matrix flips to QUICKDRAW_FALLBACK with note |
| `fill-opacity` / `stroke-opacity` | SVG only | matrix flips to INTENTIONALLY_UNSUPPORTED until SVG content handler exists |

Tests: extend `tests/css/word_break.html` with `break-all` + `keep-all`
cards; add `tests/css/list_style_types.html`; add
`tests/css/pointer_events.html`.

Acceptance:

- Matrix rows above flip from PARSED_NOT_CONSUMED to FULL / PARTIAL /
  QUICKDRAW_FALLBACK / INTENTIONALLY_UNSUPPORTED.
- No new `.c` files in libcss — all changes in NetSurf
  layout/redraw/box-construct or are matrix-only re-classifications.

---

## Track D — fixes203: Grid completion

Three items, in priority order. Use the `cssh_css.c` preprocessor
path wherever feasible to dodge libcss bit-packing risk.

1. **`grid-template-areas`** — preprocessor lowers the area-grid into
   a parallel array of `grid-area: <areaName>` declarations on
   matching children, plus a derived `grid-template-rows` /
   `-columns` count. Storage: piggyback on existing
   `macsurf_grid_tracks` + a new outer-struct
   `macsurf_grid_areas` pointer with an arena comparator (mirrors
   `grid_tracks` from fixes118).
2. **`align-items` / `justify-items`** on grid containers — already
   parsed (flex shares the accessor). Consume in `layout_grid.c`:
   per-cell alignment within its track rectangle. Stretch (the
   default) and start/end/center are the four to ship.
3. **`grid-row`** placement — mirror the `grid-column`
   preprocessor from fixes151 (cssh_css.c rewrite to
   `-macsurf-grid-row-span`).

Deferred to a later Grid V3 round: named lines,
`grid-auto-rows/columns`, `grid-auto-flow: dense`, subgrid.

Tests: `tests/css/grid_areas.html`, `tests/css/grid_alignment.html`,
`tests/css/grid_row.html`.

---

## Track E — fixes204: Multi-column

Real layout work, no libcss work needed (everything is already parsed).

1. **`column-count` / `column-width`** — new layout phase in
   `layout_block_context` that, when a block has `column-count > 1`
   or `column-width != auto`, divides its content into N equal-width
   columns by re-flowing inline content with a narrower
   `available_width`. Sequential fill is acceptable V1; balancing
   deferred.
2. **`column-gap`** — already in storage; consume here.
3. **`column-rule-*`** — paint a vertical rule between columns at
   redraw time. Uses existing `border-*-style` plotter logic.
4. **`column-span: all`** — element spans across all columns, breaks
   the multi-col flow.
5. **`column-fill`**: V1 always `auto` (sequential). `balance`
   deferred.

Tests: `tests/css/multicolumn.html` with PROBE MC1 (2-col text),
MC2 (3-col with rule), MC3 (column-span:all), MC4 (column-width
auto-determines count from container).

Acceptance: news/blog-style pages with `column-count: 2` render as
two columns instead of one tall block.

---

## Track F — fixes205: Selectors + at-rules pack

Selectors (matrix rows in A.2):

1. `:enabled` / `:disabled` / `:checked` — wire into the form-state
   plumbing; on OS 9 we only have `<input type=checkbox>` and
   `<input type=text>` rendered, so the matcher only needs to inspect
   the corresponding DOM property.
2. `:not(<selector-list>)` — extend selector matcher to iterate the
   list. (Parser already accepts.)
3. `::first-line` / `::first-letter` — defer; needs inline-line
   snapshot.

At-rules:

1. **`@supports`** — make answers track real consumption, not just
   parse-success. Reference the matrix table at compile time:
   embed a static set of (property, value-keyword) pairs known to be
   honoured. On unknown ⇒ return false rather than true. (Currently
   any parseable declaration returns true.)
2. **`@font-face` local()** — register a system-font name lookup.
   `url(...)` sources stay INTENTIONALLY_UNSUPPORTED.
3. **`@keyframes`** — parse + cascade so animation declarations
   referencing them don't error. Actual playback is Track I.

Tests: `tests/css/selector_not_list.html`, `tests/css/checked.html`,
`tests/css/supports.html`, `tests/css/font_face_local.html`.

---

## Track G — fixes206: Tables completion

1. **`table-layout: fixed`** — implement the fixed algorithm in
   `table.c`. First-row widths determine column widths.
2. **`caption-side: top / bottom`** — add `BOX_TABLE_CAPTION`
   ordering. Current renderer treats captions as siblings.
3. **Percent column widths** — verified across colspan/rowspan
   pages.
4. **`empty-cells: hide` regression guards** — extend
   `tests/css/tables.html`.

---

## Track H — fixes207: Paint / effects completion

1. **`text-shadow` blur radius** — Gaussian blur is not viable in
   QuickDraw. Treat blur > 0 as a paint-twice pattern at offsets
   (±1, ±1) to approximate. Document QUICKDRAW_FALLBACK.
2. **`box-shadow: inset`** — paint the shadow against the inner edge
   instead of the outer. Reuses fixes175 code; adds a flag in storage.
3. **`outline-offset`** — extend outline painting to honour offset.
   Parser exists (already? confirm via audit).
4. **`object-position`** — extend `html_redraw_apply_object_fit`
   (fixes116) to honour a 2-value position.
5. **Standard `transform` keyword** — re-attempt the bridge through
   the `cssh_css.c` preprocessor (text rewrite to
   `-macsurf-transform`). **Not** through `property_handlers[]`
   aliasing (fixes141 confirmed that path is broken). Use the
   fixes175 pattern.

---

## Track I — fixes208: Transitions / animations feasibility

Lowest priority because it's high-risk and low real-world-page-impact
for a static-paint browser.

1. **`transition`** — parse + accept + ignore safely. Document as
   QUICKDRAW_FALLBACK ("transitions complete instantaneously").
   This is enough to stop blocking layout on pages that declare
   them.
2. **`animation` + `@keyframes`** — V1 supports opacity-only and
   transform: rotate-only keyframes via the existing
   `-macsurf-animation-*` infrastructure. Bridge through `cssh_css.c`.
3. **`will-change`** — flip matrix row to QUICKDRAW_FALLBACK; no-op.

---

## Cross-cutting work

These are pulled into whichever track is shipping that round:

- **Probe coverage backfill.** Several rows already say FULL but
  have no probe. Each track must also add probes for any FULL row
  it touches en passant.
- **`tools/audit/` refresh** at the start and end of every round.
  Diff drives matrix updates.
- **`cssh_css.c` preprocessor**: the fixes175 pattern is the
  standards bridge for any property where the standard name and the
  vendor name differ. Keep it the single chokepoint — don't sprinkle
  alias attempts elsewhere.

## Definition of done for the whole program

`tools/audit/parsed_not_consumed.txt` is empty OR every entry has a
matrix row marked INTENTIONALLY_UNSUPPORTED with reason.

The matrix has no `unknown` cells.

`tests/css/` has at least one probe per row that says FULL or
PARTIAL.

CLAUDE.md "Current blockers" CSS section is empty.
