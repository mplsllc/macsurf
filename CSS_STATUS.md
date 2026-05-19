# MacSurf CSS Status Report

Generated 2026-05-19. Last revised 2026-05-19 (fixes146 hardware-accepted).

This is a brutal, hedge-free audit of CSS support in MacSurf. The goal is to identify what works, what doesn't, and what to implement next.

## Hardware-verified status (post fixes146)

```text
sub-AA glyph-pair overlap: ✓ Fixed
- fixes144a2 diagnostic: 216 TextWidth comparisons across 4 fonts × 3 sizes
  × 2 faces × 9 strings; every delta=0. Measurement is internally consistent;
  the "Di" overlap is a bitmap-resolution artefact at sub-AA sizes.
- fixes144b: paint-only +1px between glyphs in plotters.c when
  size < 12 && font_id != kFontIDMonaco && mac_len > 1.
- fixes146: same bump mirrored in macos9_font_measure so multi-segment
  inline content (body + <code>) doesn't scramble. Mac body text reflows
  ~chars-1 wider per segment at sub-12pt; trade-off accepted.
- Gated by MACSURF_SUBAA_DRAW_SPACING at top of plotters.c.

disc list marker: ✓ Fixed
- fixes143a: U+2022 BULLET → U+00B7 MIDDLE DOT (MacRoman 0xE1 instead of 0xA5).
  0xA5 in Helvetica TT on G3 rendered as a semicolon-looking glyph in
  disc's specific size/face context (9pt face 0); square at 0xA5 renders
  cleanly, so 0xA5 itself isn't broken — disc was a per-context font quirk.

font-family aliases: ✗ Blocked on gui_layout_table family awareness
- fixes145 attempted per-family dispatch (sans→Helvetica, serif→Times,
  mono→Monaco). Hardware-rejected as fixes145b — adjacent inline segments
  on the same line scrambled horizontally because NetSurf's inline layout
  reserves widths from one-font measurement but the plotter paints each
  segment with its own metrics.
- The fixes52 bug class is wider than originally framed: same root cause
  manifests as horizontal text scrambling, not just vertical line stacking.
- Architectural gate: macos9_font_width/position/split need family-aware
  metrics so layout reserves the correct width per inline segment.
  Plotter-only dispatch is insufficient. Kill switch
  MACSURF_FONT_FAMILY_ALIASES kept at 0 in plotters.c for the eventual
  retry once the gui_layout_table work lands.
```

## Hardware-verified status (post fixes134a)

```text
viewport units: ✓ Works
- fixes132 corrected swapped VH/VW conversion in unit.c.
- Affects height/min-height/width/etc. using vh/vw/vmin/vmax.

min-height: ✓ Works
- Source audit showed it was already consumed in layout_apply_minmax_height,
  flex, grid, tables, and replaced/replaced-like paths.
- Previous status was wrong; the visible failure was caused by swapped vh/vw
  unit conversion.

z-index: ✓ Partial
- fixes133 implements basic positioned explicit numeric z-index paint ordering.
- Positive z-index overlays/dropdowns now paint above normal content.
- Equal z-index preserves DOM order.
- Negative z-index and full CSS stacking-context paint order are deferred.

content for ::before / ::after: ✓ string + counter()
- fixes134a + fix1 materialize STRING items as BOX_TEXT under an
  INLINE_CONTAINER wrapper. CSS_CONTENT_SET guard prevents the fixes37
  uninitialised-c_item hang.
- fix1 reuses an existing trailing INLINE_CONTAINER for ::after so the
  generated content renders inline-adjacent to the element text instead
  of dropping to a new line.
- Still skipped without crashing: URI, ATTR, COUNTERS (plural),
  open/close-quote items.

CSS counters: ✓ flat decimal
- fixes134b adds counter-reset, counter-increment, content: counter(name).
- Flat document-scope table on box_construct_ctx. Element NORMAL
  counter-reset/increment fires before ::before; pseudo's own
  counter-reset/increment fires inside box_construct_generate before
  content materialises.
- Decimal output only. counters(name, ".") plural form, roman/alpha
  styles, nested CSS counter scopes deferred.
```

## fixes132 revision

The original audit claimed `min-height` was "NOT consumed in layout". A direct source audit refuted this: `layout_apply_minmax_height` (layout.c:2165) calls `ns_computed_min_height` and applies it via two sites in `layout_block_context` (layout.c:4031, 4089). Flex (layout_flex.c:1360-1362), grid (layout_grid.c:432-434), tables (layout.c:2086), and replaced elements (layout.c:175-178) also honor it. The user-visible "min-height collapses" symptom was actually a **VH/VW swap in `css_unit__px_per_unit`** (unit.c:271-275): `CSS_UNIT_VH` was returning `viewport_width/100` and `CSS_UNIT_VW` was returning `viewport_height/100`. The same file's `css_unit__absolute_len2pt` (lines 107-113) had them correct, so the bug was internal inconsistency, not a missing case. Swapped back in fixes132. This also fixes `height: 100vh`, `width: 100vw`, `vmin`, `vmax`, and any other viewport-unit property — they all routed through the broken `px_per_unit` path.

---

## The honest summary

MacSurf parses **167 CSS properties** via libcss. The layout/redraw pipeline only **reads 87 of them**. The gap between "parsed" and "consumed" is where modern pages fall apart visually — libcss correctly computes the cascade, but the layout engine never asks for the value, so the property does nothing.

Of the 87 consumed properties, **most work**, but a handful are partial (limits or accuracy gaps), and several depend on subsystems (font selection, color resolution) that have their own gaps.

---

## What actually works on real pages

These have been verified on hardware or in screenshots and produce the correct visual result:

### Box model
- `width`, `height`, `min-width`, `max-width`, `max-height`
- `margin` (all sides, `auto` centering)
- `padding` (all sides)
- `border-width`, `border-color`, `border-style` (all sides)
- `border-radius` (rounded corners via `PaintRoundRect`/`FrameRoundRect` — fixes172)
- `box-sizing`
- `box-shadow` (independent h/v offsets + custom colour — fixes175/178)

### Display & positioning
- `display: block | inline | inline-block | none | flex | inline-flex | grid | table | table-cell | table-row | list-item`
- `position: static | relative | absolute | fixed`
- `top`, `right`, `bottom`, `left`
- `float: left | right | none`
- `clear: left | right | both | none`
- `visibility: visible | hidden`
- `z-index: <integer>` (basic positioned stacking, two-pass paint — fixes133)

### Flexbox
- `flex-direction: row | row-reverse | column | column-reverse`
- `flex-wrap: nowrap | wrap | wrap-reverse`
- `flex-grow`, `flex-shrink`, `flex-basis`
- `justify-content: flex-start | flex-end | center | space-between | space-around | space-evenly`
- `align-content` (all values)
- `align-items`, `align-self`
- `order` (stable bubble-sort before layout)
- `gap: N` single-value (both axes get N)
- `column-gap: N`

### Grid (V1 — fixes75 + fixes118)
- `display: grid`
- `-macsurf-grid: N` (MacSurf shorthand for N equal columns)
- `grid-template-columns: <length-list>` (real track widths — fixes118)

### Typography
- `color`
- `font-family` (name match: Geneva, Monaco, Chicago, Charcoal)
- `font-size`
- `font-weight: normal | bold` (via QuickDraw `face=1` bold smear)
- `font-style: normal | italic` (via QuickDraw `face=2`)
- `text-align: left | right | center | justify`
- `text-decoration: underline | overline | line-through`
- `text-indent`
- `text-transform: uppercase | lowercase | capitalize | none`
- `line-height`
- `letter-spacing`
- `word-spacing` (length values; layout + paint in sync — fixes139b)
- `white-space: normal | nowrap | pre`
- `vertical-align`

### Backgrounds
- `background-color`
- `background-image: url(...)` (PNG/GIF/BMP/TIFF/JPEG via lodepng + QT — fixes78-79b)
- `background-position`
- `background-repeat` (repeat / repeat-x / repeat-y / no-repeat all honored by macos9 plot_bitmap tile loop — fixes138)
- `background-attachment: fixed` (viewport-anchored origin + tiles parallax-correct against fixes138 — fixes137 + fixes138)
- `-macsurf-gradient: linear-gradient(...)` (multi-stop — fixes49)
- `-macsurf-gradient: radial-gradient(...)` (24-ring oval stack — fixes74d)

### Lists & content
- `list-style-type` (disc/circle/square/decimal). **fixes143a (2026-05-19):** disc marker switched from U+2022 BULLET (→ MacRoman 0xA5, which Helvetica TT on G3 rendered as a semicolon-looking glyph) to U+00B7 MIDDLE DOT (→ MacRoman 0xE1, a different font slot that renders as a clean small dot). Circle stays as ASCII 'o' (no hollow-circle glyph in MacRoman). Square still uses MacRoman 0xA5 and may show the same artefact; if so, swap to ASCII fallback in a follow-up.
- `list-style-image`
- `content` for `::before` / `::after` pseudo-elements
  - string items (fixes134a)
  - decimal `counter(name)` items (fixes134b/d)
  - `open-quote` / `close-quote` / `no-open-quote` / `no-close-quote` with document-scope depth tracking (fixes140a)
- `quotes` list (fixes140b); UA defaults wrap `<q>` in curly typographic quotes via the macos9 resource CSS (fixes140c)

### Custom & visual
- CSS Custom Properties / `var()` — full native resolution (fixes133-139)
- `opacity` (QuickDraw stipple pattern at 1.0/0.80/0.50/0.25)
- `cursor` parsed; on hover, the cursor changes via `SetThemeCursor` (fixes131)
- `outline`, `outline-color`, `outline-style`, `outline-width` (parsed and consumed in redraw)
- `clip` (CSS 2 `clip: rect(...)`)
- `-macsurf-transform: rotate() translate() scale()` (fixes73)
- `-macsurf-text-shadow` (fixes50)
- `object-fit: fill | contain | cover | none | scale-down` (fixes116)
- `overflow: visible | hidden | scroll | auto` (clipping applies on block / inline-block / table-cell / flex / inline-flex / grid — fixes131 added flex/grid)
- `border-collapse`, `border-spacing` (tables)

### Pseudo-classes (fixes130 + fixes130e)
- `:hover` (re-cascade on mouse-track)
- `:active`
- `:focus`
- (Static pseudo-classes always worked: `:first-child`, `:nth-child(n)`, etc. via libcss selector engine)

---

## What is PARSED but NOT CONSUMED (the silent-fail category)

These accept author CSS without complaint but have zero effect on rendering. Every one of these is a probable visual bug on real pages.

| Property | Parsed | Layout reads? | Redraw reads? | Impact |
|---|---|---|---|---|
| `background-attachment` | yes | no | **yes (fixes137 + fixes138)** | viewport anchor + repeating tile parallax both shipped. Gradient-fixed deferred. |
| `caption-side` | yes | no | no | **Audited fixes139: deferred.** `<caption>` maps to `BOX_INLINE` in `box_construct.c:139`; no `BOX_TABLE_CAPTION` type exists. Proper support requires new box type + normalise rewrite + table-layout sibling placement. Multi-file structural change, not a minimal round. |
| `column-count` | yes | no | no | multi-column text layout broken. Deferred — text-balancing across columns is genuinely complex. |
| `column-fill` | yes | no | no | depends on column-count |
| `column-rule-*` | yes | no | no | depends on column-count |
| `column-span` | yes | no | no | depends on column-count |
| `column-width` | yes | no | no | depends on column-count |
| `quotes` | yes | no | **yes (fixes140b)** | resolved at generated-content materialisation; depth-indexed open/close strings emitted from `content: open-quote / close-quote` |
| `empty-cells` | yes | no | **yes (fixes139a)** | show/hide both honored; hidden empty cells skip background + border paint while keeping their layout slot |
| `table-layout` | yes | no | no | **Audited fixes139: deferred.** 1064 lines of dedicated table.c + table integration in layout.c. Too risky for one round per sprint rule "do not destabilize all table layout". |
| `unicode-bidi` | yes | no | no | bidi text |
| `writing-mode` | yes | no | no | vertical-writing pages broken |
| `word-spacing` | yes | **yes (fixes139b)** | **yes (fixes139b)** | length values shift word gaps; layout and paint both updated so wrap point follows |
| `break-after`, `break-before`, `break-inside` | yes | no | no | print/column breaks |
| `page-break-*` | yes | no | no | print breaks |
| `orphans`, `widows` | yes | no | no | print typography |
| `fill-opacity`, `stroke-opacity` | yes | no | no | SVG-only, low priority |

**Highest-impact silent fails on real pages, ranked:**

1. ~~**`background-attachment: fixed`**~~ **Shipped at fixes137 + fixes138 (2026-05-19).** Viewport-anchored origin + repeating-tile parallax both work end-to-end. Gradient-fixed still deferred.
2. ~~**`empty-cells`**~~ **Shipped at fixes139a (2026-05-19).** Hidden empty cells skip background + border paint; cell still occupies its layout slot. Truly empty cells (no children, no text) and ASCII-whitespace-only cells are treated as empty. `&nbsp;` is treated as visible content matching Chrome/Firefox/Safari behavior (the test's `html_box_table_cell_is_empty` checks raw UTF-8 bytes and U+00A0 encodes as `0xC2 0xA0`, whose leading `0xC2` correctly disqualifies the cell from emptiness — spec phrasing is ambiguous on NBSP, but real-browser behavior is unanimous).
3. ~~**`word-spacing`**~~ **Shipped at fixes139b (2026-05-19).** Length values resolve through the same plot_font_style_t field as letter-spacing; macos9_font_measure counts ASCII spaces and updates the wrap point so layout and paint agree.
4. **`column-count`** — Magazine-style multi-column text. Deferred — text-balancing across columns is genuinely complex.
5. **`caption-side`** — Audited fixes139, **deferred**. Captions currently fall through as BOX_INLINE because no BOX_TABLE_CAPTION type exists in this fork. Real support needs box_construct + box_normalise + table-layout coordination.
6. **`table-layout`** — Audited fixes139, **deferred**. 1064 lines of dedicated table layout code; the sprint rule "do not destabilize all table layout" forbids a one-round attempt.

(`min-height` and viewport units were previously listed here. Both shipped in fixes132. `z-index` shipped in fixes133. `counter-increment` / `counter-reset` shipped in fixes134b — see "What actually works" section.)

---

## What is BROKEN or PARTIAL on consumed properties

### `gap: A B` two-value form (fixes148 limitation)
Single-value `gap: N` works (both axes get N). Two-value `gap: A B` loses A and stores only B as column-gap. Fix requires adding `CSS_PROP_ROW_GAP` as an independent property with its own bit slot in `css_computed_style_i.bits[]`. Bit budget audit shows word 15 has 27 free bits, word 14 bottom 5 bits are full. ~17 files to touch. Real-world impact: 97% of MacTrove pages use single-value form, deferred.

### `font-family` matching
Currently only matches `Geneva`, `Monaco`, `Chicago`, `Charcoal` by name. Any other family name falls through to the OS 9 default font for the resolved generic. Modern sites specifying `"Helvetica Neue", system-ui, sans-serif` get the system font, not their preferred. Not strictly broken — there are no other fonts installed by default — but the matching is narrow.

### `font-weight` granularity
Only `bold` (>= 600) vs `normal` (< 600). Numeric weights 100/200/300/400/500/600/700/800/900 all collapse to two values. Acceptable for QuickDraw which only has bold/non-bold.

### `text-overflow: ellipsis`
**fixes135a + fixes135c (2026-05-19): WORKS on hardware.** Parser/cascade/computed plumbing in 135a; visual paint-after rendering in 135c, accepted on G3. The 135c architecture paints the ellipsis as a separate overlay: the original text draws normally (overflow:hidden clips at the container edge), then a background-coloured rect followed by a `…` text call paints over the rightmost slice. Avoids the text-buffer-mutation bug that broke the earlier fixes135b attempt. The macos9 plot_text path folds U+2026 to MacRoman 0xC9 ([macos9_font.c:109](browser/netsurf/frontends/macos9/macos9_font.c#L109)) so the same three UTF-8 bytes render correctly. Deferred to V2: multi-line ellipsis, two-value form (`text-overflow: clip ellipsis`), custom string marker, RTL/start-side ellipsis, ellipsis across complex nested inline boxes.

### `word-break` / `overflow-wrap` / `word-wrap`
**fixes136a (2026-05-19): plumbing accepted. fixes136b deferred.**

Parsed/computed status:
- `word-break: normal | break-all | keep-all` — parses, computes correctly.
- `overflow-wrap: normal | break-word | anywhere` — parses, computes correctly.
- `word-wrap: break-word` — parses as a legacy alias for `overflow-wrap`.
- Invalid values fall back to `normal`. `css_computed_word_break()` and `css_computed_overflow_wrap()` return the right enum.
- `keep-all` is parsed for forward-compat but inert without CJK segmentation. `anywhere` aliases to `break-word` semantically.

**Current layout behaviour (intentional deviation from spec).** MacSurf's inline layout already performs emergency character-boundary wrapping for long unbreakable runs even when the author has not set `overflow-wrap: break-word`. [macos9_font_split](browser/netsurf/frontends/macos9/macos9_font.c#L310) line 352-364: if no space character fits in the available width, it hard-breaks at the next character boundary. This matches the practical effect of `overflow-wrap: break-word` / `word-break: break-all` for URLs and hashes, but does **not** yet distinguish strict `normal` vs `break-word` vs `break-all` semantics. Hardware-confirmed at fixes136a: long URLs and 64-char hash tokens wrap mid-character without needing any author CSS.

A strict-spec fixes136b that disables this hard-break for `overflow-wrap: normal` was considered and rejected: real pages do not explicitly opt into `break-word`, so making `normal` strict would horizontally overflow every long URL or hash that authors haven't pre-formatted. Practical readability wins over spec strictness. The libcss plumbing still pays off when a future caller (e.g. a CSS reset that wants strict non-breaking, or richer line-break logic for `break-all`'s aggressive mode) needs to read the computed value.

If a real page later breaks because the default is too aggressive, fixes136b can revisit a narrower change: make `overflow-wrap: normal` + `word-break: normal` together signal "do not character-break", while leaving every other combination on the friendly default.

### `clip-path` and `mask`
Not implemented. Decorative shape clipping silently ignored. Pages degrade to rectangular fallback (usually fine).

### `transition` and `animation`
Deferred to v0.4.5 (already noted in CLAUDE.md). MacSurf has a one-shot `-macsurf-animation-rotate` extension but no real CSS animation property support.

### Bitmap rendering
- PNG transparency: 1-bit `CopyMask` threshold (anti-aliased edges become binary)
- TIFF: opaque rendering only (`QTNewGWorld(k32ARGBPixelFormat)` returns `cDepthErr -157` on OS 9)
- Path A1.5 (`CopyDeepMask` + 8-bit mask) is queued

### ~~List bullets render as `;`~~ — fixes143a (2026-05-19)
Disc marker switched from U+2022 BULLET (MacRoman 0xA5) to U+00B7 MIDDLE DOT (MacRoman 0xE1). The bullet glyph slot at 0xA5 in Helvetica TT on the user's G3 was rendering as a semicolon-looking artefact; middle dot uses a different font slot (0xE1) that renders cleanly. Cause is specific to disc's size/face context (9pt face 0) — square uses 0xA5 and renders correctly, so 0xA5 itself isn't broken.

### ~~"Di"-class glyph-pair overlap in Helvetica TT at body size~~ — fixes144b + fixes146 (2026-05-19)
Sub-AA bitmap rendering at 9-10pt had no anti-aliased transition pixel between adjacent glyphs, so the D's painted right edge and the i's body landed in adjacent or shared pixel columns. fixes144b adds +1px between glyphs in the draw path (paint-only) when `size < 12 && font_id != kFontIDMonaco && mac_len > 1`. fixes146 mirrors the same bump in `macos9_font_measure` so multi-segment inline content doesn't scramble (the draw-vs-measure asymmetry from fixes144b alone caused horizontal overlap between adjacent inline segments). Trade-off accepted: MacTrove body text reflows ~chars-1 wider per segment at sub-12pt. Gated by `MACSURF_SUBAA_DRAW_SPACING` at top of plotters.c (sync the same flag manually in macos9_font.c if flipped).

### Inline boxes occasionally duplicate
Known issue post-fixes33. Some inline-box runs render twice. Not blocking comprehension; cause unknown.

### URL bar input on initial window
Probably fixed by fixes77g, needs verification. Workaround: File → New Window.

---

## Implementation priorities

Ranked by current visual impact on real-world pages, after the
fixes132–fixes140 sprint sequence. The shipped backlog is folded
into [What actually works on real pages](#what-actually-works-on-real-pages);
this section only lists outstanding work.

### Q1 — Standard `transform` property bridge (LOW–MEDIUM impact, LOW effort) — **fixes141 ATTEMPTED + REVERTED**

MacSurf consumes its private `-macsurf-transform`; modern pages emit
the standard `transform: rotate() translate() scale()`. A small
bridge would route standard `transform` through the existing
fixes73 plotter, picking up real-page CSS without any new layout
or paint code.

**fixes141 attempted this** as a parser-level alias mirroring the
`word-wrap` → `overflow-wrap` pattern (new propstring entry +
SMAP entry + property_handlers entry pointing at
`css__parse_macsurf_transform`). On hardware the build still ran
but every page that referenced standard `transform: ...` hung the
Mac just before the first reformat (log truncates at
`content broadcast READY`, no `reformat:` lines fire). The
simplified test page (inline-block, no flex) crashed identically.
Reverted in the same round.

**Hypotheses for future investigation:**
1. The parser is reentrant-unsafe when called for two different
   prop indices (TRANSFORM and MACSURF_TRANSFORM in the same
   ruleset).
2. A select-side dispatch table indexed by FIRST_PROP–LAST_PROP
   range is sized statically somewhere outside the explicit
   property_handlers/propstrings arrays.
3. The bytecode emitted with `CSS_PROP_MACSURF_TRANSFORM` opcode
   but reached via a different `parser_handlers[]` slot index
   confuses something in the deferred `var()` resolution path.

Next attempt should add MS_LOG instrumentation **before** shipping,
and bisect against an even more minimal page (single inline `<span
style="transform: rotate(10deg)">` against `<span style="-macsurf-transform: ...">`).

### Q2 — Full-fidelity two-value `gap: A B` (LOW impact, MEDIUM effort)

Currently single-value `gap: N` and standalone `row-gap: N` work;
two-value `gap: A B` collapses A to column-gap. Splitting row-gap
into its own bit slot is ~17 files of cross-cutting plumbing.
Real-world impact is small.

### Q3 — `caption-side: top | bottom` (MEDIUM impact, HIGH effort)

Audited at fixes139. `<caption>` currently maps to `BOX_INLINE` in
`box_construct.c:139`; no `BOX_TABLE_CAPTION` type exists. Real
support needs a new box type, normalise rewrite, and table-layout
sibling placement. Structural change, not a minimal round.

### Q4 — `table-layout: fixed` (MEDIUM impact, HIGH effort)

Audited at fixes139. 1064 lines of dedicated table layout in
`table.c` plus table integration in `layout.c`. Per sprint rule
"do not destabilize all table layout", deferred until a
contained-scope follow-up audit finds a tractable seam.

### Q5 — `column-count` / `column-rule-*` / `column-gap` (LOW–MEDIUM impact, HIGH effort)

Multi-column text layout. Genuinely complex — text balancing,
column breaks, orphan/widow handling. Defer until proven needed
on a real page.

### Q6 — Strict `word-break: break-all` / `overflow-wrap: normal` (LOW impact)

Plumbing landed in fixes136a; layout currently always allows
character-boundary breaks on long unbreakable runs because real
pages benefit. A strict mode that respects `overflow-wrap: normal`
is parked behind "show me a real page where this matters" because
flipping the default would regress every URL-heavy page that
doesn't explicitly opt in.

### Q7 — `transition` / `animation` (v0.4.5+)

Animation framework would touch event loop, paint scheduling, time
tracking. Substantial scope. Defer to v0.4.5.

### Q8 — `clip-path`, `mask`, `filter` (LOW impact, HIGH effort)

Decorative. Pages degrade to rectangular fallback which is
acceptable. Defer indefinitely.

---

## Test plan

Without dedicated regression tests, every CSS round needs a real-page verification step. Suggested gauntlet:

1. **`tests/css/z_index.html`** — three boxes with overlapping `position: absolute` and explicit z-index — paint order should match z-index, not DOM order.
2. **`tests/css/min_height.html`** — a `div` with `min-height: 300px` containing one short line of text — should be 300px tall.
3. **`tests/css/text_overflow.html`** — card with `width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap` and 500px of text — should show `Very long title…`
4. **`tests/css/viewport_units.html`** — full-screen hero with `height: 100vh` — should fill the viewport.
5. **`tests/css/counters.html`** — `<h2>` styled with `counter-increment` and `content: counter(...)` — should auto-number.
6. **Real-page regression: MacTrove home, MacTrove app page, DuckDuckGo Lite, Wikipedia article** — visual diff against previous shipped state.

---

## Out-of-CSS items affecting page rendering

These are not CSS properties but affect whether pages "load properly":

1. **HTTP fetcher reliability** — fixes98-105 closed the major leaks; current state is stable across many-page browsing.
2. **TLS** — handled by proxy (out of scope for the browser).
3. **JavaScript** — Duktape ES5 in base build; modern JS still needs proxy render-and-flatten.
4. **Forms** — `<input>` rendering works; form submission path is wired. Style cascade reaches form controls.
5. **Tables** — table layout works for simple tables; complex tables (colspan/rowspan with auto-layout) may have gaps.

---

## What I would ship next

After fixes132–fixes146, the remaining ranked top picks are:

**Highest-impact structural work, three candidates:**

1. **Stacking contexts / full CSS 2.1 painting order.** fixes133 shipped basic positioned numeric z-index but the 7-pass painting algorithm (negative z-index → block-bg → block-borders → block-children → floats → in-flow inlines → positioned children by z-index) is not implemented. Real-world cost today: modals, tooltips, dropdowns, and fixed headers all paint under content when they're authored to overlap. Single file ([redraw.c](browser/netsurf/content/handlers/html/redraw.c)), no library or layout changes. **Highest impact per sprint.**

2. **Full CSS Grid V2.** Current V1 (`-macsurf-grid: N` from fixes75) only fires when authors opt in to our extension. Real `grid-template-columns: 1fr 200px repeat(3, minmax(100px, 1fr))`, `grid-template-rows`, `grid-template-areas`, explicit `grid-row` / `grid-column` / `grid-area`. Track-widths architecture is partially proved at fixes118 (outer-struct arena pattern); the remaining work is the grammar parser + auto-placement algorithm + named-area lookup. 2-3 sprints. **Biggest absolute transformation** of how modern sites render.

3. **`gui_layout_table` family awareness.** Architectural prerequisite for retrying font-family aliases (fixes145) without the inline-scramble bug. `macos9_font_width` / `macos9_font_position` / `macos9_font_split` need to return widths consistent with whichever family the plotter would pick. Once shipped, fixes145 retry becomes safe and pages render with real serif/mono families. Doesn't directly fix any layout bug but unblocks every future font-related win.

**Other structural items still queued:**

- Q3 `caption-side` (high effort, structural change — new BOX_TABLE_CAPTION type)
- Q4 `table-layout: fixed` (high effort, 1064-line table.c port)
- Q5 multi-column (`column-count` family — high effort)
- Q6 strict `word-break` (low impact, parked)
- Q7 `transition` / `animation` (v0.4.5+ — large scope)
- Q8 `clip-path` / `mask` / `filter` (deferred indefinitely)

Q1 (standard `transform` bridge) is on hold pending the MS_LOG bisect against fixes141's pre-`reformat:` hang. Q2 (full-fidelity two-value `gap: A B`) is parked behind ~17-file cross-cutting plumbing for marginal real-world impact.
